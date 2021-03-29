// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2021 Hyunchul Lee <hyc.lee@gmail.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "exfat_ondisk.h"
#include "libexfat.h"

#include "inode.h"
#include "exfat_fs.h"
#include "de_iter.h"
#include "fsck.h"
#include "repair.h"

static struct path_resolve_ctx path_resolve_ctx;

#define fsck_err(parent, inode, fmt, ...)		\
({							\
		resolve_path_parent(&path_resolve_ctx,	\
			parent, inode);			\
		exfat_err("ERROR: %s: " fmt,		\
			path_resolve_ctx.local_path,	\
			##__VA_ARGS__);			\
})

/*
 * try to find the dentry set matched with @filter. this function
 * doesn't verify the dentry set.
 *
 * if found, return 0. if not found, return EOF. otherwise return errno.
 */
int exfat_lookup_dentry_set(struct exfat *exfat, struct exfat_inode *parent,
			  struct exfat_lookup_filter *filter)
{
	struct buffer_desc *bd = NULL;
	struct exfat_dentry *dentry;
	off_t free_offset = 0;
	struct exfat_de_iter de_iter;
	int dentry_count;
	int retval;
	bool last_is_free = false;

	bd = exfat_alloc_buffer(2, exfat->clus_size, exfat->sect_size);
	if (!bd)
		return -ENOMEM;

	retval = exfat_de_iter_init(&de_iter, exfat, parent, bd);
	if (retval == EOF || retval)
		goto out;

	filter->out.dentry_set = NULL;
	while (1) {
		retval = exfat_de_iter_get(&de_iter, 0, &dentry);
		if (retval == EOF) {
			break;
		} else if (retval) {
			fsck_err(parent->parent, parent,
				"failed to get a dentry. %d\n", retval);
			goto out;
		}

		dentry_count = 1;
		if (dentry->type == filter->in.type) {
			retval = 0;
			if (filter->in.filter)
				retval = filter->in.filter(&de_iter,
							filter->in.param,
							&dentry_count);

			if (retval == 0) {
				struct exfat_dentry *d;
				int i;

				filter->out.dentry_set = calloc(dentry_count,
						sizeof(struct exfat_dentry));
				if (filter->out.dentry_set == NULL) {
					retval = -ENOMEM;
					goto out;
				}
				for (i = 0; i < dentry_count; i++) {
					exfat_de_iter_get(&de_iter, i, &d);
					memcpy(filter->out.dentry_set + i, d,
					       sizeof(struct exfat_dentry));
				}
				filter->out.dentry_count = dentry_count;
				goto out;
			} else if (retval < 0)
				goto out;
			last_is_free = false;
		} else if ((dentry->type == EXFAT_LAST ||
			    IS_EXFAT_DELETED(dentry->type))) {
			if (!last_is_free) {
				free_offset = exfat_de_iter_device_offset(
						&de_iter);
				last_is_free = true;
			}
		} else
			last_is_free = false;

		exfat_de_iter_advance(&de_iter, dentry_count);
	}

out:
	if (retval == 0)
		filter->out.dentry_d_offset =
			exfat_de_iter_device_offset(&de_iter);
	else if (retval == EOF && last_is_free)
		filter->out.dentry_d_offset = free_offset;
	else
		filter->out.dentry_d_offset = EOF;
	if (bd)
		exfat_free_buffer(bd, 2);
	return retval;
}
