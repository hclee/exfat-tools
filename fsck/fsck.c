// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2019 Namjae Jeon <linkinjeon@kernel.org>
 *   Copyright (C) 2020 Hyunchul Lee <hyc.lee@gmail.com>
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <locale.h>

#include "exfat_ondisk.h"
#include "libexfat.h"

#include "inode.h"
#include "exfat_fs.h"
#include "de_iter.h"
#include "fsck.h"
#include "repair.h"

struct fsck_user_input {
	struct exfat_user_input		ei;
	enum fsck_ui_options		options;
};

#define EXFAT_MAX_UPCASE_CHARS	0x10000

#define FSCK_EXIT_NO_ERRORS		0x00
#define FSCK_EXIT_CORRECTED		0x01
#define FSCK_EXIT_NEED_REBOOT		0x02
#define FSCK_EXIT_ERRORS_LEFT		0x04
#define FSCK_EXIT_OPERATION_ERROR	0x08
#define FSCK_EXIT_SYNTAX_ERROR		0x10
#define FSCK_EXIT_USER_CANCEL		0x20
#define FSCK_EXIT_LIBRARY_ERROR		0x80

struct exfat_stat {
	long		dir_count;
	long		file_count;
	long		error_count;
	long		fixed_count;
};

struct exfat_fsck exfat_fsck;
struct exfat_stat exfat_stat;
struct path_resolve_ctx path_resolve_ctx;

static struct option opts[] = {
	{"repair",	no_argument,	NULL,	'r' },
	{"repair-yes",	no_argument,	NULL,	'y' },
	{"repair-no",	no_argument,	NULL,	'n' },
	{"repair-auto",	no_argument,	NULL,	'p' },
	{"version",	no_argument,	NULL,	'V' },
	{"verbose",	no_argument,	NULL,	'v' },
	{"help",	no_argument,	NULL,	'h' },
	{"?",		no_argument,	NULL,	'?' },
	{NULL,		0,		NULL,	 0  }
};

static void usage(char *name)
{
	fprintf(stderr, "Usage: %s\n", name);
	fprintf(stderr, "\t-r | --repair        Repair interactively\n");
	fprintf(stderr, "\t-y | --repair-yes    Repair without ask\n");
	fprintf(stderr, "\t-n | --repair-no     No repair\n");
	fprintf(stderr, "\t-p | --repair-auto   Repair automatically\n");
	fprintf(stderr, "\t-a                   Repair automatically\n");
	fprintf(stderr, "\t-V | --version       Show version\n");
	fprintf(stderr, "\t-v | --verbose       Print debug\n");
	fprintf(stderr, "\t-h | --help          Show help\n");

	exit(FSCK_EXIT_SYNTAX_ERROR);
}

#define fsck_err(parent, inode, fmt, ...)		\
({							\
		resolve_path_parent(&path_resolve_ctx,	\
			parent, inode);			\
		exfat_err("ERROR: %s: " fmt,		\
			path_resolve_ctx.local_path,	\
			##__VA_ARGS__);			\
})

#define repair_file_ask(iter, inode, code, fmt, ...)	\
({							\
		resolve_path_parent(&path_resolve_ctx,	\
				(iter)->parent, inode);	\
		exfat_repair_ask(&exfat_fsck, code,	\
			"ERROR: %s: " fmt,		\
			path_resolve_ctx.local_path,	\
			##__VA_ARGS__);			\
})

static int check_clus_chain(struct exfat_de_iter *de_iter,
			    struct exfat_inode *node)
{
	struct exfat *exfat = de_iter->exfat;
	struct exfat_dentry *stream_de;
	clus_t clus, prev, next;
	uint64_t count, max_count;

	clus = node->first_clus;
	prev = EXFAT_EOF_CLUSTER;
	count = 0;
	max_count = DIV_ROUND_UP(node->size, exfat->clus_size);

	if (node->size == 0 && node->first_clus == EXFAT_FREE_CLUSTER)
		return 0;

	/* the first cluster is wrong */
	if ((node->size == 0 && node->first_clus != EXFAT_FREE_CLUSTER) ||
		(node->size > 0 && !heap_clus(exfat, node->first_clus))) {
		if (repair_file_ask(de_iter, node,
			ER_FILE_FIRST_CLUS, "first cluster is wrong"))
			goto truncate_file;
		else
			return -EINVAL;
	}

	while (clus != EXFAT_EOF_CLUSTER) {
		if (count >= max_count) {
			if (node->is_contiguous)
				break;
			if (repair_file_ask(de_iter, node,
					ER_FILE_SMALLER_SIZE,
					"more clusters are allocated. "
					"truncate to %" PRIu64 " bytes",
					count * exfat->clus_size))
				goto truncate_file;
			else
				return -EINVAL;
		}

		/*
		 * This cluster is already allocated. it may be shared with
		 * the other file, or there is a loop in cluster chain.
		 */
		if (exfat_bitmap_get(exfat->alloc_bitmap, clus)) {
			if (repair_file_ask(de_iter, node,
					ER_FILE_DUPLICATED_CLUS,
					"cluster is already allocated for "
					"the other file. truncated to %"
					PRIu64 " bytes",
					count * exfat->clus_size))
				goto truncate_file;
			else
				return -EINVAL;
		}

		if (!exfat_bitmap_get(exfat->disk_bitmap, clus)) {
			if (repair_file_ask(de_iter, node,
					ER_FILE_INVALID_CLUS,
					"cluster is marked as free. truncate to %" PRIu64 " bytes",
					count * exfat->clus_size))
				goto truncate_file;

			else
				return -EINVAL;
		}

		/* This cluster is allocated or not */
		if (get_inode_next_clus(exfat, node, clus, &next))
			goto truncate_file;
		if (!node->is_contiguous) {
			if (!heap_clus(exfat, next) &&
					next != EXFAT_EOF_CLUSTER) {
				if (repair_file_ask(de_iter, node,
						ER_FILE_INVALID_CLUS,
						"broken cluster chain. "
						"truncate to %"
						PRIu64 " bytes",
						count * exfat->clus_size))
					goto truncate_file;

				else
					return -EINVAL;
			}
		}

		count++;
		exfat_bitmap_set(exfat->alloc_bitmap, clus);
		prev = clus;
		clus = next;
	}

	if (count < max_count) {
		if (repair_file_ask(de_iter, node,
			ER_FILE_LARGER_SIZE, "less clusters are allocated. "
			"truncates to %" PRIu64 " bytes",
			count * exfat->clus_size))
			goto truncate_file;
		else
			return -EINVAL;
	}

	return 0;
truncate_file:
	node->size = count * exfat->clus_size;
	if (!heap_clus(exfat, prev))
		node->first_clus = EXFAT_FREE_CLUSTER;

	exfat_de_iter_get_dirty(de_iter, 1, &stream_de);
	if (count * exfat->clus_size <
			le64_to_cpu(stream_de->stream_valid_size))
		stream_de->stream_valid_size = cpu_to_le64(
				count * exfat->clus_size);
	if (!heap_clus(exfat, prev))
		stream_de->stream_start_clu = EXFAT_FREE_CLUSTER;
	stream_de->stream_size = cpu_to_le64(
			count * exfat->clus_size);

	/* remaining clusters will be freed while FAT is compared with
	 * alloc_bitmap.
	 */
	if (!node->is_contiguous && heap_clus(exfat, prev))
		return set_fat(exfat, prev, EXFAT_EOF_CLUSTER);
	return 1;
}

static bool root_get_clus_count(struct exfat *exfat, struct exfat_inode *node,
							clus_t *clus_count)
{
	clus_t clus;

	clus = node->first_clus;
	*clus_count = 0;

	do {
		if (!heap_clus(exfat, clus)) {
			exfat_err("/: bad cluster. 0x%x\n", clus);
			return false;
		}

		if (exfat_bitmap_get(exfat->alloc_bitmap, clus)) {
			exfat_err("/: cluster is already allocated, or "
				"there is a loop in cluster chain\n");
			return false;
		}

		exfat_bitmap_set(exfat->alloc_bitmap, clus);

		if (get_inode_next_clus(exfat, node, clus, &clus) != 0) {
			exfat_err("/: broken cluster chain\n");
			return false;
		}

		(*clus_count)++;
	} while (clus != EXFAT_EOF_CLUSTER);
	return true;
}

static int boot_region_checksum(struct exfat_blk_dev *bd, int bs_offset)
{
	void *sect;
	unsigned int i;
	uint32_t checksum;
	int ret = 0;
	unsigned int size;

	size = bd->sector_size;
	sect = malloc(size);
	if (!sect)
		return -ENOMEM;

	checksum = 0;
	for (i = 0; i < 11; i++) {
		if (exfat_read(bd->dev_fd, sect, size,
				bs_offset * size + i * size) !=
				(ssize_t)size) {
			exfat_err("failed to read boot region\n");
			ret = -EIO;
			goto out;
		}
		boot_calc_checksum(sect, size, i == 0, &checksum);
	}

	if (exfat_read(bd->dev_fd, sect, size,
			bs_offset * size + 11 * size) !=
			(ssize_t)size) {
		exfat_err("failed to read a boot checksum sector\n");
		ret = -EIO;
		goto out;
	}

	for (i = 0; i < size/sizeof(checksum); i++) {
		if (le32_to_cpu(((__le32 *)sect)[i]) != checksum) {
			exfat_err("checksum of boot region is not correct. %#x, but expected %#x\n",
				le32_to_cpu(((__le32 *)sect)[i]), checksum);
			ret = -EINVAL;
			goto out;
		}
	}
out:
	free(sect);
	return ret;
}

static int exfat_mark_volume_dirty(struct exfat *exfat, bool dirty)
{
	uint16_t flags;

	flags = le16_to_cpu(exfat->bs->bsx.vol_flags);
	if (dirty)
		flags |= 0x02;
	else
		flags &= ~0x02;

	exfat->bs->bsx.vol_flags = cpu_to_le16(flags);
	if (exfat_write(exfat->blk_dev->dev_fd, exfat->bs,
			sizeof(struct pbr), 0) != (ssize_t)sizeof(struct pbr)) {
		exfat_err("failed to set VolumeDirty\n");
		return -EIO;
	}

	if (fsync(exfat->blk_dev->dev_fd) != 0) {
		exfat_err("failed to set VolumeDirty\n");
		return -EIO;
	}
	return 0;
}

static int read_boot_region(struct exfat_blk_dev *bd, struct pbr **pbr,
		int bs_offset)
{
	struct pbr *bs;
	int ret = -EINVAL;

	*pbr = NULL;
	bs = (struct pbr *)malloc(sizeof(struct pbr));
	if (!bs) {
		exfat_err("failed to allocate memory\n");
		return -ENOMEM;
	}

	if (exfat_read(bd->dev_fd, bs, sizeof(*bs),
			bs_offset * bd->sector_size) != (ssize_t)sizeof(*bs)) {
		exfat_err("failed to read a boot sector\n");
		ret = -EIO;
		goto err;
	}

	if (memcmp(bs->bpb.oem_name, "EXFAT   ", 8) != 0) {
		exfat_err("failed to find exfat file system.\n");
		goto err;
	}

	ret = boot_region_checksum(bd, bs_offset);
	if (ret < 0)
		goto err;

	ret = -EINVAL;
	if (EXFAT_SECTOR_SIZE(bs) < 512 || EXFAT_SECTOR_SIZE(bs) > 4 * KB) {
		exfat_err("too small or big sector size: %d\n",
				EXFAT_SECTOR_SIZE(bs));
		goto err;
	}

	if (EXFAT_CLUSTER_SIZE(bs) > 32 * MB) {
		exfat_err("too big cluster size: %d\n", EXFAT_CLUSTER_SIZE(bs));
		goto err;
	}

	if (bs->bsx.fs_version[1] != 1 || bs->bsx.fs_version[0] != 0) {
		exfat_err("unsupported exfat version: %d.%d\n",
				bs->bsx.fs_version[1], bs->bsx.fs_version[0]);
		goto err;
	}

	if (bs->bsx.num_fats != 1) {
		exfat_err("unsupported FAT count: %d\n", bs->bsx.num_fats);
		goto err;
	}

	if (le64_to_cpu(bs->bsx.vol_length) * EXFAT_SECTOR_SIZE(bs) >
			bd->size) {
		exfat_err("too large sector count: %" PRIu64 ", expected: %llu\n",
				le64_to_cpu(bs->bsx.vol_length),
				bd->num_sectors);
		goto err;
	}

	if (le32_to_cpu(bs->bsx.clu_count) * EXFAT_CLUSTER_SIZE(bs) >
			bd->size) {
		exfat_err("too large cluster count: %u, expected: %u\n",
				le32_to_cpu(bs->bsx.clu_count),
				bd->num_clusters);
		goto err;
	}

	*pbr = bs;
	return 0;
err:
	free(bs);
	return ret;
}

static int restore_boot_region(struct exfat_blk_dev *bd)
{
	int i;
	char *sector;
	int ret;

	sector = malloc(bd->sector_size);
	if (!sector)
		return -ENOMEM;

	for (i = 0; i < 12; i++) {
		if (exfat_read(bd->dev_fd, sector, bd->sector_size,
				BACKUP_BOOT_SEC_IDX * bd->sector_size +
				i * bd->sector_size) !=
				(ssize_t)bd->sector_size) {
			ret = -EIO;
			goto free_sector;
		}
		if (i == 0)
			((struct pbr *)sector)->bsx.perc_in_use = 0xff;

		if (exfat_write(bd->dev_fd, sector, bd->sector_size,
				BOOT_SEC_IDX * bd->sector_size +
				i * bd->sector_size) !=
				(ssize_t)bd->sector_size) {
			ret = -EIO;
			goto free_sector;
		}
	}

	if (fsync(bd->dev_fd)) {
		ret = -EIO;
		goto free_sector;
	}
	ret = 0;

free_sector:
	free(sector);
	return ret;
}

static int exfat_boot_region_check(struct exfat_blk_dev *blkdev, struct pbr **bs)
{
	int ret;

	ret = read_boot_region(blkdev, bs, BOOT_SEC_IDX);
	if (ret == -EINVAL && exfat_repair_ask(&exfat_fsck, ER_BS_BOOT_REGION,
		"boot region is corrupted. try to restore the region from backup")) {
		ret = read_boot_region(blkdev, bs, BACKUP_BOOT_SEC_IDX);
		if (ret < 0) {
			exfat_err("backup boot region is also corrupted\n");
			return ret;
		}
		ret = restore_boot_region(blkdev);
		if (ret < 0) {
			exfat_err("failed to restore boot region from backup\n");
			free(*bs);
			*bs = NULL;
			return ret;
		}
	}
	return ret;
}

static uint16_t file_calc_checksum(struct exfat_de_iter *iter)
{
	uint16_t checksum;
	struct exfat_dentry *file_de, *de;
	int i;

	checksum = 0;
	exfat_de_iter_get(iter, 0, &file_de);

	exfat_calc_dentry_checksum(file_de, &checksum, true);
	for (i = 1; i <= file_de->file_num_ext; i++) {
		exfat_de_iter_get(iter, i, &de);
		exfat_calc_dentry_checksum(de, &checksum, false);
	}
	return checksum;
}

/*
 * return 0 if there are no errors, or 1 if errors are fixed, or
 * an error code
 */
static int check_inode(struct exfat_de_iter *iter, struct exfat_inode *node)
{
	struct exfat *exfat = iter->exfat;
	struct exfat_dentry *dentry;
	int ret = 0;
	uint16_t checksum;
	bool valid = true;

	ret = check_clus_chain(iter, node);
	if (ret < 0)
		return ret;

	if (node->size > le32_to_cpu(exfat->bs->bsx.clu_count) *
				(uint64_t)exfat->clus_size) {
		fsck_err(iter->parent, node,
			"size %" PRIu64 " is greater than cluster heap\n",
			node->size);
		valid = false;
	}

	if (node->size == 0 && node->is_contiguous) {
		if (repair_file_ask(iter, node, ER_FILE_ZERO_NOFAT,
				"empty, but has no Fat chain\n")) {
			exfat_de_iter_get_dirty(iter, 1, &dentry);
			dentry->stream_flags &= ~EXFAT_SF_CONTIGUOUS;
			ret = 1;
		} else
			valid = false;
	}

	if ((node->attr & ATTR_SUBDIR) &&
			node->size % exfat->clus_size != 0) {
		fsck_err(iter->parent, node,
			"directory size %" PRIu64 " is not divisible by %d\n",
			node->size, exfat->clus_size);
		valid = false;
	}

	checksum = file_calc_checksum(iter);
	exfat_de_iter_get(iter, 0, &dentry);
	if (checksum != le16_to_cpu(dentry->file_checksum)) {
		if (repair_file_ask(iter, node, ER_DE_CHECKSUM,
				"the checksum of a file is wrong")) {
			exfat_de_iter_get_dirty(iter, 0, &dentry);
			dentry->file_checksum = cpu_to_le16(checksum);
			ret = 1;
		} else
			valid = false;
	}

	return valid ? ret : -EINVAL;
}

static int read_file_dentries(struct exfat_de_iter *iter,
			struct exfat_inode **new_node, int *skip_dentries)
{
	struct exfat_dentry *file_de, *stream_de, *name_de;
	struct exfat_inode *node;
	int i, ret;

	/* TODO: mtime, atime, ... */

	ret = exfat_de_iter_get(iter, 0, &file_de);
	if (ret || file_de->type != EXFAT_FILE) {
		exfat_err("failed to get file dentry. %d\n", ret);
		return -EINVAL;
	}
	ret = exfat_de_iter_get(iter, 1, &stream_de);
	if (ret || stream_de->type != EXFAT_STREAM) {
		exfat_err("failed to get stream dentry. %d\n", ret);
		return -EINVAL;
	}

	*new_node = NULL;
	node = alloc_exfat_inode(le16_to_cpu(file_de->file_attr));
	if (!node)
		return -ENOMEM;

	if (file_de->file_num_ext < 2) {
		exfat_err("too few secondary count. %d\n",
				file_de->file_num_ext);
		free_exfat_inode(node);
		return -EINVAL;
	}

	for (i = 2; i <= file_de->file_num_ext; i++) {
		ret = exfat_de_iter_get(iter, i, &name_de);
		if (ret || name_de->type != EXFAT_NAME) {
			exfat_err("failed to get name dentry. %d\n", ret);
			ret = -EINVAL;
			goto err;
		}

		memcpy(node->name +
			(i-2) * ENTRY_NAME_MAX, name_de->name_unicode,
			sizeof(name_de->name_unicode));
	}

	node->first_clus = le32_to_cpu(stream_de->stream_start_clu);
	node->is_contiguous =
		((stream_de->stream_flags & EXFAT_SF_CONTIGUOUS) != 0);
	node->size = le64_to_cpu(stream_de->stream_size);

	if (node->size < le64_to_cpu(stream_de->stream_valid_size)) {
		if (repair_file_ask(iter, node, ER_FILE_VALID_SIZE,
			"valid size %" PRIu64 " greater than size %" PRIu64,
			le64_to_cpu(stream_de->stream_valid_size),
			node->size)) {
			exfat_de_iter_get_dirty(iter, 1, &stream_de);
			stream_de->stream_valid_size =
					stream_de->stream_size;
		} else {
			ret = -EINVAL;
			goto err;
		}
	}

	*skip_dentries = (file_de->file_num_ext + 1);
	*new_node = node;
	return 0;
err:
	*skip_dentries = 0;
	*new_node = NULL;
	free_exfat_inode(node);
	return ret;
}

static int read_file(struct exfat_de_iter *de_iter,
		struct exfat_inode **new_node, int *dentry_count)
{
	struct exfat_inode *node;
	int ret;

	*new_node = NULL;

	ret = read_file_dentries(de_iter, &node, dentry_count);
	if (ret)
		return ret;

	ret = check_inode(de_iter, node);
	if (ret < 0) {
		free_exfat_inode(node);
		return -EINVAL;
	}

	if (node->attr & ATTR_SUBDIR)
		exfat_stat.dir_count++;
	else
		exfat_stat.file_count++;
	*new_node = node;
	return ret;
}

static bool read_volume_label(struct exfat_de_iter *iter)
{
	struct exfat *exfat;
	struct exfat_dentry *dentry;
	__le16 disk_label[VOLUME_LABEL_MAX_LEN];

	exfat = iter->exfat;
	if (exfat_de_iter_get(iter, 0, &dentry))
		return false;

	if (dentry->vol_char_cnt == 0)
		return true;

	if (dentry->vol_char_cnt > VOLUME_LABEL_MAX_LEN) {
		exfat_err("too long label. %d\n", dentry->vol_char_cnt);
		return false;
	}

	memcpy(disk_label, dentry->vol_label, sizeof(disk_label));
	if (exfat_utf16_dec(disk_label, dentry->vol_char_cnt*2,
		exfat->volume_label, sizeof(exfat->volume_label)) < 0) {
		exfat_err("failed to decode volume label\n");
		return false;
	}

	exfat_info("volume label [%s]\n", exfat->volume_label);
	return true;
}

static int read_bitmap(struct exfat *exfat)
{
	struct exfat_lookup_filter filter = {
		.in.type	= EXFAT_BITMAP,
		.in.filter	= NULL,
		.in.param	= NULL,
	};
	struct exfat_dentry *dentry;
	int retval;

	retval = exfat_lookup_dentry_set(exfat, exfat->root, &filter);
	if (retval)
		return retval;

	dentry = filter.out.dentry_set;
	exfat_debug("start cluster %#x, size %#" PRIx64 "\n",
			le32_to_cpu(dentry->bitmap_start_clu),
			le64_to_cpu(dentry->bitmap_size));

	if (le64_to_cpu(dentry->bitmap_size) <
			DIV_ROUND_UP(exfat->clus_count, 8)) {
		exfat_err("invalid size of allocation bitmap. 0x%" PRIx64 "\n",
				le64_to_cpu(dentry->bitmap_size));
		return -EINVAL;
	}
	if (!heap_clus(exfat, le32_to_cpu(dentry->bitmap_start_clu))) {
		exfat_err("invalid start cluster of allocate bitmap. 0x%x\n",
				le32_to_cpu(dentry->bitmap_start_clu));
		return -EINVAL;
	}

	exfat->disk_bitmap_clus = le32_to_cpu(dentry->bitmap_start_clu);
	exfat->disk_bitmap_size = DIV_ROUND_UP(exfat->clus_count, 8);

	exfat_bitmap_set_range(exfat, exfat->alloc_bitmap,
			       le32_to_cpu(dentry->bitmap_start_clu),
			       DIV_ROUND_UP(exfat->disk_bitmap_size,
					    exfat->clus_size));
	free(filter.out.dentry_set);

	if (exfat_read(exfat->blk_dev->dev_fd, exfat->disk_bitmap,
			exfat->disk_bitmap_size,
			exfat_c2o(exfat, exfat->disk_bitmap_clus)) !=
			(ssize_t)exfat->disk_bitmap_size)
		return -EIO;
	return 0;
}

static int decompress_upcase_table(const __le16 *in_table, size_t in_len,
				    __u16 *out_table, size_t out_len)
{
	int i, k;
	uint16_t ch;

	if (in_len > out_len)
		return -E2BIG;

	i = 0;
	while (i < (int)in_len) {
		ch = le16_to_cpu(in_table[i]);

		if (ch == 0xFFFF && i + 1 < (int)in_len) {
			int len = (int)le16_to_cpu(in_table[i + 1]);

			for (k = 0; k < len; k++)
				out_table[i + k] = (uint16_t)(i + k);
			i += len;
		} else
			out_table[i++] = ch;
	}

	for (; i < (int)out_len; i++)
		out_table[i] = (uint16_t)i;
	return 0;
}

static int read_upcase_table(struct exfat *exfat)
{
	struct exfat_lookup_filter filter = {
		.in.type	= EXFAT_UPCASE,
		.in.filter	= NULL,
		.in.param	= NULL,
	};
	struct exfat_dentry *dentry = NULL;
	__le16 *upcase = NULL;
	int retval;
	ssize_t size;
	__le32 checksum;

	retval = exfat_lookup_dentry_set(exfat, exfat->root, &filter);
	if (retval)
		return retval;

	dentry = filter.out.dentry_set;

	if (!heap_clus(exfat, le32_to_cpu(dentry->upcase_start_clu))) {
		exfat_err("invalid start cluster of upcase table. 0x%x\n",
			le32_to_cpu(dentry->upcase_start_clu));
		retval = -EINVAL;
		goto out;
	}

	size = (ssize_t)le64_to_cpu(dentry->upcase_size);
	if (size > (ssize_t)(EXFAT_MAX_UPCASE_CHARS * sizeof(__le16)) ||
			size == 0 || size % sizeof(__le16)) {
		exfat_err("invalid size of upcase table. 0x%" PRIx64 "\n",
			le64_to_cpu(dentry->upcase_size));
		retval = -EINVAL;
		goto out;
	}

	upcase = (__le16 *)malloc(size);
	if (!upcase) {
		exfat_err("failed to allocate upcase table\n");
		retval = -ENOMEM;
		goto out;
	}

	if (exfat_read(exfat->blk_dev->dev_fd, upcase, size,
			exfat_c2o(exfat,
			le32_to_cpu(dentry->upcase_start_clu))) != size) {
		exfat_err("failed to read upcase table\n");
		retval = -EIO;
		goto out;
	}

	checksum = 0;
	boot_calc_checksum((unsigned char *)upcase, size, false, &checksum);
	if (le32_to_cpu(dentry->upcase_checksum) != checksum) {
		exfat_err("corrupted upcase table %#x (expected: %#x)\n",
			checksum, le32_to_cpu(dentry->upcase_checksum));
		retval = -EINVAL;
		goto out;
	}

	exfat_bitmap_set_range(exfat, exfat->alloc_bitmap,
			       le32_to_cpu(dentry->upcase_start_clu),
			       DIV_ROUND_UP(le64_to_cpu(dentry->upcase_size),
					    exfat->clus_size));

	exfat->upcase_table = calloc(1,
				sizeof(uint16_t) * EXFAT_UPCASE_TABLE_CHARS);
	if (exfat->upcase_table == NULL) {
		retval = -EIO;
		goto out;
	}

	decompress_upcase_table(upcase, size/2,
				exfat->upcase_table, EXFAT_UPCASE_TABLE_CHARS);
out:
	if (dentry)
		free(dentry);
	if (upcase)
		free(upcase);
	return retval;
}

static int read_children(struct exfat_fsck *fsck, struct exfat_inode *dir)
{
	struct exfat *exfat = fsck->exfat;
	struct exfat_inode *node = NULL;
	struct exfat_dentry *dentry;
	struct exfat_de_iter *de_iter;
	int dentry_count;
	int ret;

	de_iter = &fsck->de_iter;
	ret = exfat_de_iter_init(de_iter, exfat, dir, fsck->buffer_desc);
	if (ret == EOF)
		return 0;
	else if (ret)
		return ret;

	while (1) {
		ret = exfat_de_iter_get(de_iter, 0, &dentry);
		if (ret == EOF) {
			break;
		} else if (ret) {
			fsck_err(dir->parent, dir,
				"failed to get a dentry. %d\n", ret);
			goto err;
		}

		dentry_count = 1;

		switch (dentry->type) {
		case EXFAT_FILE:
			ret = read_file(de_iter, &node, &dentry_count);
			if (ret < 0) {
				exfat_stat.error_count++;
				break;
			} else if (ret) {
				exfat_stat.error_count++;
				exfat_stat.fixed_count++;
			}

			if ((node->attr & ATTR_SUBDIR) && node->size) {
				node->parent = dir;
				list_add_tail(&node->sibling, &dir->children);
				list_add_tail(&node->list, &exfat->dir_list);
			} else
				free_exfat_inode(node);
			break;
		case EXFAT_VOLUME:
			if (!read_volume_label(de_iter)) {
				exfat_err("failed to verify volume label\n");
				ret = -EINVAL;
				goto err;
			}
			break;
		case EXFAT_BITMAP:
		case EXFAT_UPCASE:
			break;
		case EXFAT_LAST:
			goto out;
		default:
			if (!IS_EXFAT_DELETED(dentry->type))
				exfat_err("unknown entry type. 0x%x\n",
					  dentry->type);
			break;
		}

		exfat_de_iter_advance(de_iter, dentry_count);
	}
out:
	exfat_de_iter_flush(de_iter);
	return 0;
err:
	inode_free_children(dir, false);
	INIT_LIST_HEAD(&dir->children);
	exfat_de_iter_flush(de_iter);
	return ret;
}

static int write_dirty_fat(struct exfat_fsck *fsck)
{
	struct exfat *exfat = fsck->exfat;
	struct buffer_desc *bd;
	off_t offset;
	ssize_t len;
	size_t read_size, write_size;
	clus_t clus, last_clus, clus_count, i;
	unsigned int idx;

	clus = 0;
	last_clus = le32_to_cpu(exfat->bs->bsx.clu_count) + 2;
	bd = fsck->buffer_desc;
	idx = 0;
	offset = le32_to_cpu(exfat->bs->bsx.fat_offset) *
		exfat->sect_size;
	read_size = exfat->clus_size;
	write_size = exfat->sect_size;

	while (clus < last_clus) {
		clus_count = MIN(read_size / sizeof(clus_t), last_clus - clus);
		len = exfat_read(exfat->blk_dev->dev_fd, bd[idx].buffer,
				clus_count * sizeof(clus_t), offset);
		if (len != (ssize_t)(sizeof(clus_t) * clus_count)) {
			exfat_err("failed to read fat entries, %zd\n", len);
			return -EIO;
		}

		/* TODO: read ahead */

		for (i = clus ? clus : EXFAT_FIRST_CLUSTER;
				i < clus + clus_count; i++) {
			if (!exfat_bitmap_get(exfat->alloc_bitmap, i) &&
					((clus_t *)bd[idx].buffer)[i - clus] !=
					EXFAT_FREE_CLUSTER) {
				((clus_t *)bd[idx].buffer)[i - clus] =
					EXFAT_FREE_CLUSTER;
				bd[idx].dirty[(i - clus) /
					(write_size / sizeof(clus_t))] = true;
			}
		}

		for (i = 0; i < read_size; i += write_size) {
			if (bd[idx].dirty[i / write_size]) {
				if (exfat_write(exfat->blk_dev->dev_fd,
						&bd[idx].buffer[i], write_size,
						offset + i) !=
						(ssize_t)write_size) {
					exfat_err("failed to write "
						"fat entries\n");
					return -EIO;

				}
				bd[idx].dirty[i / write_size] = false;
			}
		}

		idx ^= 0x01;
		clus = clus + clus_count;
		offset += len;
	}
	return 0;
}

static int write_dirty_bitmap(struct exfat_fsck *fsck)
{
	struct exfat *exfat = fsck->exfat;
	struct buffer_desc *bd;
	off_t offset, last_offset, bitmap_offset;
	ssize_t len;
	ssize_t read_size, write_size, i, size;
	int idx;

	offset = exfat_c2o(exfat, exfat->disk_bitmap_clus);
	last_offset = offset + exfat->disk_bitmap_size;
	bitmap_offset = 0;
	read_size = exfat->clus_size;
	write_size = exfat->sect_size;

	bd = fsck->buffer_desc;
	idx = 0;

	while (offset < last_offset) {
		len = MIN(read_size, last_offset - offset);
		if (exfat_read(exfat->blk_dev->dev_fd, bd[idx].buffer,
				len, offset) != (ssize_t)len)
			return -EIO;

		/* TODO: read-ahead */

		for (i = 0; i < len; i += write_size) {
			size = MIN(write_size, len - i);
			if (memcmp(&bd[idx].buffer[i],
					exfat->alloc_bitmap + bitmap_offset + i,
					size)) {
				if (exfat_write(exfat->blk_dev->dev_fd,
					exfat->alloc_bitmap + bitmap_offset + i,
					size, offset + i) != size)
					return -EIO;
			}
		}

		idx ^= 0x01;
		offset += len;
		bitmap_offset += len;
	}
	return 0;
}

static int reclaim_free_clusters(struct exfat_fsck *fsck)
{
	if (write_dirty_fat(fsck)) {
		exfat_err("failed to write fat entries\n");
		return -EIO;
	}
	if (write_dirty_bitmap(fsck)) {
		exfat_err("failed to write bitmap\n");
		return -EIO;
	}
	return 0;
}

/*
 * for each directory in @dir_list.
 * 1. read all dentries and allocate exfat_nodes for files and directories.
 *    and append directory exfat_nodes to the head of @dir_list
 * 2. free all of file exfat_nodes.
 * 3. if the directory does not have children, free its exfat_node.
 */
static int exfat_filesystem_check(struct exfat_fsck *fsck)
{
	struct exfat *exfat = fsck->exfat;
	struct exfat_inode *dir;
	int ret = 0, dir_errors;

	if (!exfat->root) {
		exfat_err("root is NULL\n");
		return -ENOENT;
	}

	list_add(&exfat->root->list, &exfat->dir_list);

	while (!list_empty(&exfat->dir_list)) {
		dir = list_entry(exfat->dir_list.next,
				 struct exfat_inode, list);

		if (!(dir->attr & ATTR_SUBDIR)) {
			fsck_err(dir->parent, dir,
				"failed to travel directories. "
				"the node is not directory\n");
			ret = -EINVAL;
			goto out;
		}

		dir_errors = read_children(fsck, dir);
		if (dir_errors) {
			resolve_path(&path_resolve_ctx, dir);
			exfat_debug("failed to check dentries: %s\n",
					path_resolve_ctx.local_path);
			ret = dir_errors;
		}

		list_del(&dir->list);
		inode_free_file_children(dir);
		inode_free_ancestors(dir);
	}
out:
	exfat_free_dir_list(exfat);
	exfat->root = NULL;
	if (fsck->dirty_fat && reclaim_free_clusters(fsck))
		return -EIO;
	return ret;
}

static int exfat_root_dir_check(struct exfat *exfat)
{
	struct exfat_inode *root;
	clus_t clus_count;

	root = alloc_exfat_inode(ATTR_SUBDIR);
	if (!root) {
		exfat_err("failed to allocate memory\n");
		return -ENOMEM;
	}

	root->first_clus = le32_to_cpu(exfat->bs->bsx.root_cluster);
	if (!root_get_clus_count(exfat, root, &clus_count)) {
		exfat_err("failed to follow the cluster chain of root\n");
		free_exfat_inode(root);
		return -EINVAL;
	}
	root->size = clus_count * exfat->clus_size;

	exfat->root = root;
	exfat_stat.dir_count++;
	exfat_debug("root directory: start cluster[0x%x] size[0x%" PRIx64 "]\n",
		root->first_clus, root->size);

	if (read_bitmap(exfat)) {
		exfat_err("failed to read bitmap\n");
		return -EINVAL;
	}

	if (read_upcase_table(exfat)) {
		exfat_err("failed to read upcase table\n");
		return -EINVAL;
	}
	return 0;
}

static char *bytes_to_human_readable(size_t bytes)
{
	static const char * const units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
	static char buf[15*4];
	unsigned int i, shift, quoti, remain;

	shift = 0;
	for (i = 0; i < sizeof(units)/sizeof(units[0]); i++) {
		if (bytes / (1ULL << (shift + 10)) == 0)
			break;
		shift += 10;
	}

	quoti = (unsigned int)(bytes / (1ULL << shift));
	remain = 0;
	if (shift > 0) {
		remain = (unsigned int)
			((bytes & ((1ULL << shift) - 1)) >> (shift - 10));
		remain = (remain * 100) / 1024;
	}

	snprintf(buf, sizeof(buf), "%u.%02u %s", quoti, remain, units[i]);
	return buf;
}

static void exfat_show_info(struct exfat_fsck *fsck, const char *dev_name,
			int errors)
{
	struct exfat *exfat = fsck->exfat;

	exfat_info("sector size:  %s\n",
		bytes_to_human_readable(1 << exfat->bs->bsx.sect_size_bits));
	exfat_info("cluster size: %s\n",
		bytes_to_human_readable(exfat->clus_size));
	exfat_info("volume size:  %s\n",
		bytes_to_human_readable(exfat->blk_dev->size));

	printf("%s: %s. directories %ld, files %ld\n", dev_name,
			errors ? "checking stopped" : "clean",
			exfat_stat.dir_count, exfat_stat.file_count);
	if (errors || fsck->dirty)
		printf("%s: files corrupted %ld, files fixed %ld\n", dev_name,
			exfat_stat.error_count, exfat_stat.fixed_count);
}

int main(int argc, char * const argv[])
{
	struct fsck_user_input ui;
	struct exfat_blk_dev bd;
	struct pbr *bs = NULL;
	int c, ret, exit_code;
	bool version_only = false;

	memset(&ui, 0, sizeof(ui));
	memset(&bd, 0, sizeof(bd));

	print_level = EXFAT_ERROR;

	if (!setlocale(LC_CTYPE, ""))
		exfat_err("failed to init locale/codeset\n");

	opterr = 0;
	while ((c = getopt_long(argc, argv, "arynpVvh", opts, NULL)) != EOF) {
		switch (c) {
		case 'n':
			if (ui.options & FSCK_OPTS_REPAIR_ALL)
				usage(argv[0]);
			ui.options |= FSCK_OPTS_REPAIR_NO;
			break;
		case 'r':
			if (ui.options & FSCK_OPTS_REPAIR_ALL)
				usage(argv[0]);
			ui.options |= FSCK_OPTS_REPAIR_ASK;
			break;
		case 'y':
			if (ui.options & FSCK_OPTS_REPAIR_ALL)
				usage(argv[0]);
			ui.options |= FSCK_OPTS_REPAIR_YES;
			break;
		case 'a':
		case 'p':
			if (ui.options & FSCK_OPTS_REPAIR_ALL)
				usage(argv[0]);
			ui.options |= FSCK_OPTS_REPAIR_AUTO;
			break;
		case 'V':
			version_only = true;
			break;
		case 'v':
			if (print_level < EXFAT_DEBUG)
				print_level++;
			break;
		case '?':
		case 'h':
		default:
			usage(argv[0]);
		}
	}

	show_version();
	if (optind != argc - 1)
		usage(argv[0]);

	if (version_only)
		exit(FSCK_EXIT_SYNTAX_ERROR);
	if (ui.options & FSCK_OPTS_REPAIR_WRITE)
		ui.ei.writeable = true;
	else {
		ui.options |= FSCK_OPTS_REPAIR_NO;
		ui.ei.writeable = false;
	}

	exfat_fsck.options = ui.options;

	snprintf(ui.ei.dev_name, sizeof(ui.ei.dev_name), "%s", argv[optind]);
	ret = exfat_get_blk_dev_info(&ui.ei, &bd);
	if (ret < 0) {
		exfat_err("failed to open %s. %d\n", ui.ei.dev_name, ret);
		return FSCK_EXIT_OPERATION_ERROR;
	}

	ret = exfat_boot_region_check(&bd, &bs);
	if (ret)
		goto err;

	exfat_fsck.exfat = exfat_alloc_exfat(&bd, bs);
	if (!exfat_fsck.exfat) {
		ret = -ENOMEM;
		goto err;
	}

	exfat_fsck.buffer_desc = exfat_alloc_buffer(2,
					exfat_fsck.exfat->clus_size,
					exfat_fsck.exfat->sect_size);
	if (!exfat_fsck.buffer_desc) {
		ret = -ENOMEM;
		goto err;
	}

	if ((exfat_fsck.options & FSCK_OPTS_REPAIR_WRITE) &&
	    exfat_mark_volume_dirty(exfat_fsck.exfat, true)) {
		ret = -EIO;
		goto err;
	}

	exfat_debug("verifying root directory...\n");
	ret = exfat_root_dir_check(exfat_fsck.exfat);
	if (ret) {
		exfat_err("failed to verify root directory.\n");
		goto out;
	}

	exfat_debug("verifying directory entries...\n");
	ret = exfat_filesystem_check(&exfat_fsck);
	if (ret)
		goto out;

	if (ui.ei.writeable && fsync(bd.dev_fd)) {
		exfat_err("failed to sync\n");
		ret = -EIO;
		goto out;
	}
	if (exfat_fsck.options & FSCK_OPTS_REPAIR_WRITE)
		exfat_mark_volume_dirty(exfat_fsck.exfat, false);

out:
	exfat_show_info(&exfat_fsck, ui.ei.dev_name, ret);
err:
	if (ret == -EINVAL)
		exit_code = FSCK_EXIT_ERRORS_LEFT;
	else if (ret)
		exit_code = FSCK_EXIT_OPERATION_ERROR;
	else if (exfat_fsck.dirty)
		exit_code = FSCK_EXIT_CORRECTED;
	else
		exit_code = FSCK_EXIT_NO_ERRORS;

	if (exfat_fsck.buffer_desc)
		exfat_free_buffer(exfat_fsck.buffer_desc, 2);
	if (exfat_fsck.exfat)
		exfat_free_exfat(exfat_fsck.exfat);
	close(bd.dev_fd);
	return exit_code;
}
