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

void exfat_calc_dentry_checksum(struct exfat_dentry *dentry,
				uint16_t *checksum, bool primary)
{
	unsigned int i;
	uint8_t *bytes;

	bytes = (uint8_t *)dentry;

	*checksum = ((*checksum << 15) | (*checksum >> 1)) + bytes[0];
	*checksum = ((*checksum << 15) | (*checksum >> 1)) + bytes[1];

	i = primary ? 4 : 2;
	for (; i < sizeof(*dentry); i++)
		*checksum = ((*checksum << 15) | (*checksum >> 1)) + bytes[i];
}

static uint16_t calc_dentry_set_checksum(struct exfat_dentry *dset, int dcount)
{
	uint16_t checksum;
	int i;

	if (dcount < MIN_FILE_DENTRIES)
		return 0;

	checksum = 0;
	exfat_calc_dentry_checksum(&dset[0], &checksum, true);
	for (i = 1; i < dcount; i++)
		exfat_calc_dentry_checksum(&dset[i], &checksum, false);
	return checksum;
}

uint16_t exfat_calc_name_hash(struct exfat *exfat,
			       __le16 *name, int len)
{
	int i;
	__le16 ch;
	uint16_t chksum = 0;

	for (i = 0; i < len; i++) {
		ch = exfat->upcase_table[le16_to_cpu(name[i])];
		ch = cpu_to_le16(ch);

		chksum = ((chksum << 15) | (chksum >> 1)) + (ch & 0xFF);
		chksum = ((chksum << 15) | (chksum >> 1)) + (ch >> 8);
	}
	return chksum;
}

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

int exfat_build_file_dentry_set(struct exfat *exfat, const char *name,
			unsigned short attr, struct exfat_dentry **dentry_set,
			int *dentry_count)
{
	struct exfat_dentry *dset;
	__le16 utf16_name[PATH_MAX + 2];
	int retval;
	int dcount, name_len, i;
	__le16 e_date, e_time;
	__u8 tz, e_time_ms;

	memset(utf16_name, 0, sizeof(utf16_name));
	retval = exfat_utf16_enc(name, utf16_name, sizeof(utf16_name));
	if (retval < 0)
		return retval;

	name_len = retval / 2;
	dcount = 2 + DIV_ROUND_UP(name_len, ENTRY_NAME_MAX);
	dset = calloc(1, dcount * DENTRY_SIZE);
	if (dset == NULL)
		return -ENOMEM;

	dset[0].type = EXFAT_FILE;
	dset[0].dentry.file.num_ext = dcount - 1;
	dset[0].dentry.file.attr = cpu_to_le16(attr);

	unix_time_to_exfat_time(time(NULL), &tz,
				&e_date, &e_time, &e_time_ms);

	dset[0].dentry.file.create_date = e_date;
	dset[0].dentry.file.create_time = e_time;
	dset[0].dentry.file.create_time_ms = e_time_ms;
	dset[0].dentry.file.create_tz = tz;

	dset[0].dentry.file.modify_date = e_date;
	dset[0].dentry.file.modify_time = e_time;
	dset[0].dentry.file.modify_time_ms = e_time_ms;
	dset[0].dentry.file.modify_tz = tz;

	dset[0].dentry.file.access_date = e_date;
	dset[0].dentry.file.access_time = e_time;
	dset[0].dentry.file.access_tz = tz;

	dset[1].type = EXFAT_STREAM;
	dset[1].dentry.stream.flags = 0x01;
	dset[1].dentry.stream.name_len = (__u8)name_len;
	dset[1].dentry.stream.name_hash = cpu_to_le16(
			exfat_calc_name_hash(exfat, utf16_name, name_len));

	for (i = 2; i < dcount; i++) {
		dset[i].type = EXFAT_NAME;
		memcpy(dset[i].dentry.name.unicode_0_14,
		       utf16_name + (i - 2) * ENTRY_NAME_MAX * 2,
		       ENTRY_NAME_MAX * 2);
	}

	dset[0].dentry.file.checksum = cpu_to_le16(
			calc_dentry_set_checksum(dset, dcount));

	*dentry_set = dset;
	*dentry_count = dcount;
	return 0;
}

/* inplace update dentry set, @dset.
 * TODO: need cleanup. replace arguments with structure and flags
 */
int exfat_update_file_dentry_set(struct exfat *exfat,
				  struct exfat_dentry *dset, int dcount,
				  const char *name,
				  clus_t start_clu, clus_t ccount)
{
	int i, name_len;
	__le16 utf16_name[PATH_MAX + 2];

	if (dset[0].type != EXFAT_FILE || dcount < 3)
		return -EINVAL;

	if (name) {
		name_len = (int)exfat_utf16_enc(name,
						utf16_name, sizeof(utf16_name));
		if (name_len < 0)
			return name_len;

		name_len /= 2;
		if (dcount != 2 + DIV_ROUND_UP(name_len, ENTRY_NAME_MAX))
			return -EINVAL;

		dset[1].dentry.stream.name_len = (__u8)name_len;
		dset[1].dentry.stream.name_hash = cpu_to_le16(
			exfat_calc_name_hash(exfat, utf16_name, name_len));

		for (i = 2; i < dcount; i++) {
			dset[i].type = EXFAT_NAME;
			memcpy(dset[i].dentry.name.unicode_0_14,
			       utf16_name + (i - 2) * ENTRY_NAME_MAX * 2,
			       ENTRY_NAME_MAX * 2);
		}
	}

	dset[1].dentry.stream.valid_size =
				cpu_to_le64(ccount * exfat->clus_size);
	dset[1].dentry.stream.size = cpu_to_le64(ccount * exfat->clus_size);
	if (start_clu)
		dset[1].dentry.stream.start_clu = cpu_to_le32(start_clu);

	dset[0].dentry.file.checksum = cpu_to_le16(
				calc_dentry_set_checksum(dset, dcount));
	return 0;
}

static int find_empty_cluster(struct exfat *exfat,
			      clus_t start, clus_t *new_clu)
{
	clus_t end = le32_to_cpu(exfat->bs->bsx.clu_count) +
		EXFAT_FIRST_CLUSTER;

	while (start < end) {
		if (exfat_find_zero_bit(exfat, exfat->alloc_bitmap,
					 start, new_clu))
			break;
		if (!exfat_bitmap_get(exfat->disk_bitmap, *new_clu))
			return 0;
		start = *new_clu + 1;
	}

	end = start;
	start = EXFAT_FIRST_CLUSTER;
	while (start < end) {
		if (exfat_find_zero_bit(exfat, exfat->alloc_bitmap,
					 start, new_clu))
			return -ENOSPC;
		if (!exfat_bitmap_get(exfat->disk_bitmap, *new_clu))
			return 0;
		start = *new_clu + 1;
	}

	*new_clu = EXFAT_EOF_CLUSTER;
	return -ENOSPC;
}

static int exfat_map_cluster(struct exfat *exfat, struct exfat_inode *inode,
		      off_t offset, clus_t *mapped_clu)
{
	clus_t clu, next, count, last_count;

	if (!heap_clus(exfat, inode->first_clus))
		return -EINVAL;

	clu = inode->first_clus;
	next = EXFAT_EOF_CLUSTER;
	count = 1;
	if (offset == EOF)
		last_count = DIV_ROUND_UP(inode->size, exfat->clus_size);
	else
		last_count = DIV_ROUND_UP(offset, exfat->clus_size);

	while (true) {
		if (count * exfat->clus_size > inode->size)
			return -EINVAL;

		if (count == last_count) {
			*mapped_clu = clu;
			return 0;
		}

		if (get_inode_next_clus(exfat, inode, clu, &next))
			return -EINVAL;

		if (!heap_clus(exfat, clu))
			return -EINVAL;

		clu = next;
		count++;
	}
	return -EINVAL;
}

/* TODO: handle contiguous allocation file */
int exfat_alloc_cluster(struct exfat *exfat, struct exfat_inode *inode,
			clus_t *new_clu, bool zero_fill)
{
	clus_t start, last_clu;
	int err;
	bool need_dset = inode != exfat->root;

	if (need_dset && (!inode->dentry_set || inode->dev_offset == 0))
		return -EINVAL;

	if (exfat->start_clu != EXFAT_EOF_CLUSTER)
		start = exfat->start_clu;
	else
		start = EXFAT_FIRST_CLUSTER;

	err = find_empty_cluster(exfat, start, new_clu);
	if (err) {
		exfat_err("failed to find an empty cluster: No space\n");
		return -ENOSPC;
	}

	exfat->start_clu = *new_clu;

	if (set_fat(exfat, *new_clu, EXFAT_EOF_CLUSTER))
		return -EIO;
	if (zero_fill) {
		if (exfat_write(exfat->blk_dev->dev_fd, exfat->zero_cluster,
				exfat->clus_size, exfat_c2o(exfat, *new_clu)) !=
				(ssize_t)exfat->clus_size) {
			exfat_err("failed to fill new cluster with zeroes\n");
			return -EIO;
		}
	}

	if (inode->size) {
		err = exfat_map_cluster(exfat, inode, EOF, &last_clu);
		if (err) {
			exfat_err("failed to get the last cluster\n");
			return err;
		}

		if (set_fat(exfat, last_clu, *new_clu))
			return -EIO;

		if (need_dset) {
			err = exfat_update_file_dentry_set(exfat,
					   inode->dentry_set,
					   inode->dentry_count,
					   NULL, 0,
					   DIV_ROUND_UP(inode->size,
							exfat->clus_size) + 1);
			if (err)
				return -EIO;
		}
	} else {
		if (need_dset) {
			err = exfat_update_file_dentry_set(exfat,
							   inode->dentry_set,
							   inode->dentry_count,
							   NULL, *new_clu, 1);
			if (err)
				return -EIO;
		}
	}

	/* TODO: handle the dentry set which locates in two clusters */
	if (need_dset &&
	    exfat_write(exfat->blk_dev->dev_fd,
			inode->dentry_set, inode->dentry_count * DENTRY_SIZE,
			inode->dev_offset) != (ssize_t)inode->dentry_count * DENTRY_SIZE)
		return -EIO;

	exfat_bitmap_set(exfat->alloc_bitmap, *new_clu);
	if (inode->size == 0)
		inode->first_clus = *new_clu;
	inode->size += exfat->clus_size;
	return 0;
}

int exfat_add_dentry_set(struct exfat *exfat, struct exfat_dentry_loc *loc,
			 struct exfat_dentry *dset, int dcount,
			 bool need_next_loc)
{
	struct exfat_inode *parent = loc->parent;
	off_t dev_offset;
	size_t size;

	/* need to read the dentry set */
	if (!parent->dentry_set || parent->dev_offset == EOF)
		return -EINVAL;

	size = dcount * DENTRY_SIZE;
	if (loc->file_offset + size >= parent->size) {
		clus_t new_clu;
		int err;

		err = exfat_alloc_cluster(exfat, parent, &new_clu, true);
		if (err) {
			exfat_err("failed to allocate a cluster\n");
			return err;
		}

		if (loc->file_offset % exfat->clus_size) {
			size = exfat->clus_size -
				loc->file_offset % exfat->clus_size;
			if (size % DENTRY_SIZE)
				return -EINVAL;

			if (exfat_write(exfat->blk_dev->dev_fd, dset, size,
					loc->dev_offset) != (ssize_t)size)
				return -EIO;

			dset = (struct exfat_dentry *)((char *)dset + size);
			size = dcount * DENTRY_SIZE - size;
		}
		dev_offset = exfat_c2o(exfat, new_clu);
	} else
		dev_offset = loc->dev_offset;

	if (exfat_write(exfat->blk_dev->dev_fd, dset, size, dev_offset) !=
	    (ssize_t)size)
		return -EIO;

	if (need_next_loc) {
		loc->file_offset += dcount * DENTRY_SIZE;
		loc->dev_offset = dev_offset + size;
	}
	return 0;
}

int exfat_create_file(struct exfat *exfat, struct exfat_inode *parent,
		      const char *name, unsigned short attr,
		      struct exfat_dentry **out_dset, int *out_dcount,
		      off_t *out_dev_offset)
{
	struct exfat_dentry *dset;
	int err, dcount;
	struct exfat_lookup_filter filter;
	struct exfat_dentry_loc loc;

	err = exfat_lookup_file(exfat, parent, name, &filter);
	if (err == 0) {
		struct exfat_dentry *dent;

		dent = &filter.out.dentry_set[0];
		if ((le16_to_cpu(dent->dentry.file.attr) & attr) != attr) {
			free(filter.out.dentry_set);
			return -EEXIST;
		}

		dset = filter.out.dentry_set;
		dcount = filter.out.dentry_count;
		goto out;
	}

	err = exfat_build_file_dentry_set(exfat, name, attr,
					  &dset, &dcount);
	if (err)
		return err;

	loc.parent = parent;
	loc.file_offset = filter.out.file_offset;
	loc.dev_offset = filter.out.dev_offset;
	err = exfat_add_dentry_set(exfat, &loc, dset, dcount, true);
	if (err) {
		free(dset);
		return err;
	}
out:
	if (out_dset) {
		*out_dset = dset;
		*out_dcount = dcount;
	} else
		free(dset);
	if (out_dev_offset) {
		if (filter.out.dev_offset != EOF)
			*out_dev_offset = filter.out.dev_offset;
		else
			*out_dev_offset = loc.dev_offset - dcount * DENTRY_SIZE;
	}
	return 0;
}

