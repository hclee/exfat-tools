// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2021 Hyunchul Lee <hyc.lee@gmail.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include "exfat_ondisk.h"
#include "libexfat.h"

#include "inode.h"
#include "exfat_fs.h"
#include "de_iter.h"

static void unix_time_to_exfat_time(time_t unix_time, __u8 *tz, __le16 *date,
			__le16 *time, __u8 *time_ms)
{
	struct tm tm;
	__u16 t, d;

	gmtime_r(&unix_time, &tm);
	d = ((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday;
	t = (tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec >> 1);

	*tz = 0x80;
	*date = cpu_to_le16(d);
	*time = cpu_to_le16(t);
	if (time_ms)
		*time_ms = (tm.tm_sec & 1) * 100;
}

static __u16 exfat_calc_name_chksum(struct exfat *exfat, __le16 *name, int len)
{
	int i;
	__le16 ch;
	__u16 chksum = 0;

	for (i = 0; i < len; i++) {
		ch = exfat->upcase_table[le16_to_cpu(name[i])];
		ch = cpu_to_le16(ch);

		chksum = ((chksum << 15) | (chksum >> 1)) + (ch & 0xFF);
		chksum = ((chksum << 15) | (chksum >> 1)) + (ch >> 8);
	}
	return chksum;
}

int exfat_build_file_dentry_set(struct exfat *exfat, const char *name,
			unsigned short attr, struct exfat_dentry **dentry_set,
			int *dentry_count)
{
	struct exfat_dentry *d_set;
	__le16 utf16_name[PATH_MAX + 2];
	int retval;
	int d_count, name_len, i;
	__le16 e_date, e_time;
	__u8 tz, e_time_ms;

	memset(utf16_name, 0, sizeof(utf16_name));
	retval = exfat_utf16_enc(name, utf16_name, sizeof(utf16_name));
	if (retval < 0)
		return retval;

	name_len = retval / 2;
	d_count = 2 + ((name_len + ENTRY_NAME_MAX - 1) / ENTRY_NAME_MAX);
	d_set = calloc(d_count, sizeof(struct exfat_dentry));
	if (d_set == NULL)
		return -ENOMEM;

	d_set[0].type = EXFAT_FILE;
	d_set[0].dentry.file.num_ext = d_count - 1;
	d_set[0].dentry.file.attr = cpu_to_le16(attr);

	unix_time_to_exfat_time(time(NULL), &tz,
				&e_date, &e_time, &e_time_ms);

	d_set[0].dentry.file.create_date = e_date;
	d_set[0].dentry.file.create_time = e_time;
	d_set[0].dentry.file.create_time_ms = e_time_ms;
	d_set[0].dentry.file.create_tz = tz;

	d_set[0].dentry.file.modify_date = e_date;
	d_set[0].dentry.file.modify_time = e_time;
	d_set[0].dentry.file.modify_time_ms = e_time_ms;
	d_set[0].dentry.file.modify_tz = tz;

	d_set[0].dentry.file.access_date = e_date;
	d_set[0].dentry.file.access_time = e_time;
	d_set[0].dentry.file.access_tz = tz;

	d_set[1].type = EXFAT_STREAM;
	d_set[1].dentry.stream.flags = 0x01;
	d_set[1].dentry.stream.name_len = (__u8)name_len;
	d_set[1].dentry.stream.name_hash = cpu_to_le16(
			exfat_calc_name_chksum(exfat, utf16_name, name_len));

	for (i = 2; i < d_count; i++) {
		d_set[i].type = EXFAT_NAME;
		memcpy(d_set[i].dentry.name.unicode_0_14,
		       utf16_name + (i - 2) * ENTRY_NAME_MAX * 2,
		       ENTRY_NAME_MAX * 2);
	}

	*dentry_set = d_set;
	*dentry_count = d_count;
	return 0;
}

int exfat_create_file(struct exfat *exfat, struct exfat_inode *parent,
		      const char *name, unsigned short attr)
{
	struct exfat_dentry *dentry_set;
	int dentry_count;
	int retval;
	unsigned int clu, offset, set_len;
	struct exfat_lookup_filter filter;

	retval = exfat_lookup_file(exfat, parent, name,
				   &filter);
	if (retval == 0) {
		struct exfat_dentry *dentry;

		dentry = filter.out.dentry_set;
		if ((le16_to_cpu(dentry->dentry.file.attr) & attr) != attr)
			retval = -EEXIST;

		free(filter.out.dentry_set);
		return retval;
	}

	retval = exfat_build_file_dentry_set(exfat, name, attr,
				       &dentry_set, &dentry_count);
	if (retval < 0)
		return retval;

	retval = exfat_o2c(exfat, filter.out.dev_offset, &clu, &offset);
	if (retval)
		goto out;

	set_len = dentry_count * sizeof(struct exfat_dentry);
	if (offset + set_len > exfat->clus_size) {
		retval = -ENOSPC;
		goto out;
	}

	if (exfat_write(exfat->blk_dev->dev_fd, dentry_set,
			set_len, filter.out.dev_offset) !=
			(ssize_t)set_len) {
		retval = -EIO;
		goto out;
	}
out:
	free(dentry_set);
	return 0;
}
