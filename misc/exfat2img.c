// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2021 Hyunchul Lee <hyc.lee@gmail.com>
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>

#include "exfat_ondisk.h"
#include "libexfat.h"
#include "../fsck/inode.h"
#include "../fsck/exfat_fs.h"
#include "../fsck/de_iter.h"

#define EXFAT_MAX_UPCASE_CHARS	0x10000

struct exfat2img {
	int			out_fd;
	struct exfat_blk_dev	bdev;
	struct exfat		*exfat;
	struct buffer_desc	*dump_bdesc;
	struct buffer_desc	*scan_bdesc;
	struct exfat_de_iter	de_iter;
};

struct exfat_stat {
	long		dir_count;
	long		file_count;
	long		error_count;
	uint64_t	written_bytes;
};

static struct exfat2img ei;
static struct exfat_stat exfat_stat;
static struct path_resolve_ctx path_resolve_ctx;

static void usage(const char *name)
{
	fprintf(stderr, "Usage: %s device image-file\n", name);
	exit(EXIT_FAILURE);
}

#define ei_err(parent, inode, fmt, ...)			\
({							\
		resolve_path_parent(&path_resolve_ctx,	\
			parent, inode);			\
		exfat_err("ERROR: %s: " fmt,		\
			path_resolve_ctx.local_path,	\
			##__VA_ARGS__);			\
})

static void free_exfat2img(struct exfat2img *ei)
{
	if (ei->exfat)
		exfat_free_exfat(ei->exfat);
	if (ei->dump_bdesc)
		exfat_free_buffer(ei->dump_bdesc, 2);
	if (ei->scan_bdesc)
		exfat_free_buffer(ei->scan_bdesc, 2);
	if (ei->out_fd)
		close(ei->out_fd);
	if (ei->bdev.dev_fd)
		close(ei->bdev.dev_fd);
}

static int create_exfat2img(struct exfat2img *ei,
			    struct pbr *bs,
			    const char *out_path)
{
	int err;

	ei->exfat = exfat_alloc_exfat(&ei->bdev, bs);
	if (!ei->exfat)
		return -ENOMEM;

	ei->dump_bdesc = exfat_alloc_buffer(2,
					    ei->exfat->clus_size,
					    ei->exfat->sect_size);
	if (!ei->dump_bdesc) {
		err = -ENOMEM;
		goto err;
	}

	ei->scan_bdesc = exfat_alloc_buffer(2,
					    ei->exfat->clus_size,
					    ei->exfat->sect_size);
	if (!ei->scan_bdesc) {
		err = -ENOMEM;
		goto err;
	}

	ei->out_fd = open(out_path, O_CREAT | O_TRUNC | O_RDWR, 0664);
	if (ei->out_fd < 0) {
		exfat_err("failed to open %s: %s\n", out_path,
			  strerror(errno));
		err = -errno;
		goto err;
	}

	return 0;
err:
	free_exfat2img(ei);
	return err;
}

static int read_boot_sect(struct exfat_blk_dev *bdev, struct pbr **bs)
{
	struct pbr *pbr;
	int err = 0;
	unsigned int sect_size, clu_size;

	pbr = malloc(sizeof(struct pbr));

	if (exfat_read(bdev->dev_fd, pbr, sizeof(*pbr), 0) !=
	    (ssize_t)sizeof(*pbr)) {
		exfat_err("failed to read a boot sector\n");
		err = -EIO;
		goto err;
	}

	err = -EINVAL;
	if (memcmp(pbr->bpb.oem_name, "EXFAT   ", 8) != 0) {
		exfat_err("failed to find exfat file system\n");
		goto err;
	}

	sect_size = 1 << pbr->bsx.sect_size_bits;
	clu_size = 1 << (pbr->bsx.sect_size_bits +
			 pbr->bsx.sect_per_clus_bits);

	if (sect_size < 512 || sect_size > 4 * KB) {
		exfat_err("too small or big sector size: %d\n",
			  sect_size);
		goto err;
	}

	if (clu_size < sect_size || clu_size > 32 * MB) {
		exfat_err("too small or big cluster size: %d\n",
			  clu_size);
		goto err;
	}

	*bs = pbr;
	return 0;
err:
	free(pbr);
	return err;
}

/**
 * @end: excluded.
 */
static ssize_t dump_range(struct exfat2img *ei, off_t start, off_t end)
{
	struct exfat *exfat = ei->exfat;
	size_t len, total_len = 0;
	ssize_t ret;

	while (start < end) {
		len = (size_t)MIN(end - start, exfat->clus_size);

		ret = exfat_read(exfat->blk_dev->dev_fd,
				 ei->dump_bdesc[0].buffer,
				 len, start);
		if (ret != (ssize_t)len) {
			exfat_err("failed to read %llu bytes at %llu\n",
				  (unsigned long long)len,
				  (unsigned long long)start);
			return -EIO;
		}

		ret = pwrite(ei->out_fd, ei->dump_bdesc[0].buffer,
			     len, start);
		if (ret != (ssize_t)len) {
			exfat_err("failed to write %llu bytes at %llu\n",
				  (unsigned long long)len,
				  (unsigned long long)start);
			return -EIO;
		}

		start += len;
		total_len += len;
		exfat_stat.written_bytes += len;
	}
	return total_len;
}

static int dump_sectors(struct exfat2img *ei,
			off_t start_sect,
			off_t end_sect_excl)
{
	struct exfat *exfat = ei->exfat;
	off_t s, e;

	s = exfat_s2o(exfat, start_sect);
	e = exfat_s2o(exfat, end_sect_excl);
	return dump_range(ei, s, e) <= 0 ? -EIO : 0;
}

static int dump_clusters(struct exfat2img *ei,
			 clus_t start_clus,
			 clus_t end_clus_excl)
{
	struct exfat *exfat = ei->exfat;
	off_t s, e;

	s = exfat_c2o(exfat, start_clus);
	e = exfat_c2o(exfat, end_clus_excl);
	return dump_range(ei, s, e) <= 0 ? -EIO : 0;
}

static int dump_directory(struct exfat2img *ei,
			  struct exfat_inode *inode, size_t size,
			  clus_t *out_clus_count)
{
	struct exfat *exfat = ei->exfat;
	clus_t clus, possible_count;
	uint64_t max_count;
	size_t dump_size;
	off_t start_off, end_off;

	if (size == 0)
		return -EINVAL;

	if (!(inode->attr & ATTR_SUBDIR))
		return -EINVAL;

	clus = inode->first_clus;
	*out_clus_count = 0;
	max_count = DIV_ROUND_UP(inode->size, exfat->clus_size);

	possible_count = (256 * MB) >> (exfat->bs->bsx.sect_per_clus_bits +
					exfat->bs->bsx.sect_size_bits);
	possible_count = MIN(possible_count, exfat->clus_count);

	while (heap_clus(exfat, clus) && *out_clus_count < possible_count) {
		dump_size = MIN(size, exfat->clus_size);
		start_off = exfat_c2o(exfat, clus);
		end_off = start_off + DIV_ROUND_UP(dump_size, 512) * 512;

		if (dump_range(ei, start_off, end_off) < 0)
			return -EIO;

		*out_clus_count += 1;
		size -= dump_size;
		if (size == 0)
			break;

		if (inode->is_contiguous) {
			if (*out_clus_count >= max_count)
				break;
		}
		if (get_inode_next_clus(exfat, inode, clus, &clus))
			return -EINVAL;
	}
	return 0;
}

static int dump_root(struct exfat2img *ei)
{
	struct exfat *exfat = ei->exfat;
	struct exfat_inode *root;
	clus_t clus_count = 0;

	root = alloc_exfat_inode(ATTR_SUBDIR);
	if (!root)
		return -ENOMEM;

	root->first_clus = le32_to_cpu(exfat->bs->bsx.root_cluster);
	dump_directory(ei, root, (size_t)-1, &clus_count);
	root->size = clus_count * exfat->clus_size;

	ei->exfat->root = root;
	return 0;
}

static int read_file_dentry_set(struct exfat_de_iter *iter,
			struct exfat_inode **new_node, int *skip_dentries)
{
	struct exfat_dentry *file_de, *stream_de, *dentry;
	struct exfat_inode *node = NULL;
	int i, ret;

	ret = exfat_de_iter_get(iter, 0, &file_de);
	if (ret || file_de->type != EXFAT_FILE) {
		exfat_debug("failed to get file dentry\n");
		return -EINVAL;
	}

	ret = exfat_de_iter_get(iter, 1, &stream_de);
	if (ret || stream_de->type != EXFAT_STREAM) {
		exfat_debug("failed to get stream dentry\n");
		*skip_dentries = 2;
		goto skip_dset;
	}

	*new_node = NULL;
	node = alloc_exfat_inode(le16_to_cpu(file_de->file_attr));
	if (!node)
		return -ENOMEM;

	for (i = 2; i <= file_de->file_num_ext; i++) {
		ret = exfat_de_iter_get(iter, i, &dentry);
		if (ret || dentry->type != EXFAT_NAME)
			break;
		memcpy(node->name +
			(i-2) * ENTRY_NAME_MAX, dentry->name_unicode,
			sizeof(dentry->name_unicode));
	}

	node->first_clus = le32_to_cpu(stream_de->stream_start_clu);
	node->is_contiguous =
		((stream_de->stream_flags & EXFAT_SF_CONTIGUOUS) != 0);
	node->size = le64_to_cpu(stream_de->stream_size);

	*skip_dentries = i;
	*new_node = node;
	return 0;
skip_dset:
	*new_node = NULL;
	free_exfat_inode(node);
	return -EINVAL;
}

static int read_file(struct exfat_de_iter *de_iter,
		struct exfat_inode **new_node, int *dentry_count)
{
	struct exfat_inode *node;
	int ret;

	*new_node = NULL;

	ret = read_file_dentry_set(de_iter, &node, dentry_count);
	if (ret)
		return ret;

	if (node->attr & ATTR_SUBDIR)
		exfat_stat.dir_count++;
	else
		exfat_stat.file_count++;
	*new_node = node;
	return ret;
}

static int read_bitmap(struct exfat2img *ei, struct exfat_de_iter *iter)
{
	struct exfat *exfat = ei->exfat;
	struct exfat_dentry *dentry;
	int ret;

	ret = exfat_de_iter_get(iter, 0, &dentry);
	if (ret || dentry->type != EXFAT_BITMAP) {
		exfat_debug("failed to get bimtap dentry\n");
		return -EINVAL;
	}

	exfat_debug("start cluster %#x, size %#" PRIx64 "\n",
			le32_to_cpu(dentry->bitmap_start_clu),
			le64_to_cpu(dentry->bitmap_size));

	if (!heap_clus(exfat, le32_to_cpu(dentry->bitmap_start_clu))) {
		exfat_err("invalid start cluster of allocate bitmap. 0x%x\n",
				le32_to_cpu(dentry->bitmap_start_clu));
		return -EINVAL;
	}

	exfat->disk_bitmap_clus = le32_to_cpu(dentry->bitmap_start_clu);
	exfat->disk_bitmap_size = DIV_ROUND_UP(exfat->clus_count, 8);

	return dump_clusters(ei,
			     exfat->disk_bitmap_clus,
			     exfat->disk_bitmap_clus +
			     DIV_ROUND_UP(exfat->disk_bitmap_size,
					  exfat->clus_size));
}

static int read_upcase_table(struct exfat2img *ei,
			     struct exfat_de_iter *iter)
{
	struct exfat *exfat = ei->exfat;
	struct exfat_dentry *dentry = NULL;
	int retval;
	ssize_t size;

	retval = exfat_de_iter_get(iter, 0, &dentry);
	if (retval || dentry->type != EXFAT_UPCASE) {
		exfat_debug("failed to get upcase dentry\n");
		return -EINVAL;
	}

	if (!heap_clus(exfat, le32_to_cpu(dentry->upcase_start_clu))) {
		exfat_err("invalid start cluster of upcase table. 0x%x\n",
			le32_to_cpu(dentry->upcase_start_clu));
		return -EINVAL;
	}

	size = EXFAT_MAX_UPCASE_CHARS * sizeof(__le16);
	return dump_clusters(ei, le32_to_cpu(dentry->upcase_start_clu),
			     le32_to_cpu(dentry->upcase_start_clu) +
			     DIV_ROUND_UP(size, exfat->clus_size));
}

static int read_children(struct exfat2img *ei, struct exfat_inode *dir,
			 off_t *end_file_offset)
{
	struct exfat *exfat = ei->exfat;
	struct exfat_inode *node = NULL;
	struct exfat_dentry *dentry;
	struct exfat_de_iter *de_iter;
	int dentry_count;
	int ret;

	*end_file_offset = 0;
	de_iter = &ei->de_iter;
	ret = exfat_de_iter_init(de_iter, exfat, dir, ei->scan_bdesc);
	if (ret == EOF)
		return 0;
	else if (ret)
		return ret;

	while (1) {
		ret = exfat_de_iter_get(de_iter, 0, &dentry);
		if (ret == EOF) {
			break;
		} else if (ret) {
			ei_err(dir->parent, dir,
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
			}

			if (node) {
				if ((node->attr & ATTR_SUBDIR) && node->size) {
					node->parent = dir;
					list_add_tail(&node->sibling,
						      &dir->children);
					list_add_tail(&node->list,
						      &exfat->dir_list);
				} else
					free_exfat_inode(node);
			}
			break;
		case EXFAT_LAST:
			goto out;
		case EXFAT_BITMAP:
			ret = read_bitmap(ei, de_iter);
			if (ret)
				exfat_debug("failed to read bitmap\n");
			break;
		case EXFAT_UPCASE:
			ret = read_upcase_table(ei, de_iter);
			if (ret)
				exfat_debug("failed to upcase table\n");
			break;
		case EXFAT_VOLUME:
		default:
			break;
		}

		ret = exfat_de_iter_advance(de_iter, dentry_count);
	}
out:
	*end_file_offset = exfat_de_iter_file_offset(de_iter);
	exfat_de_iter_flush(de_iter);
	return 0;
err:
	inode_free_children(dir, false);
	INIT_LIST_HEAD(&dir->children);
	exfat_de_iter_flush(de_iter);
	return ret;
}

static int dump_filesystem(struct exfat2img *ei)
{
	struct exfat *exfat = ei->exfat;
	struct exfat_inode *dir;
	int ret = 0, dir_errors;
	clus_t clus_count;
	off_t end_file_offset;

	if (!exfat->root) {
		exfat_err("root is NULL\n");
		return -ENOENT;
	}

	list_add(&exfat->root->list, &exfat->dir_list);

	while (!list_empty(&exfat->dir_list)) {
		dir = list_entry(exfat->dir_list.next,
				 struct exfat_inode, list);
		clus_count = 0;

		if (!(dir->attr & ATTR_SUBDIR)) {
			ei_err(dir->parent, dir,
			       "failed to travel directories. "
			       "the node is not directory\n");
			ret = -EINVAL;
			goto out;
		}

		dir_errors = read_children(ei, dir, &end_file_offset);
		if (!dir_errors) {
			dump_directory(ei, dir, (size_t)end_file_offset,
				       &clus_count);
		} else if (dir_errors) {
			dump_directory(ei, dir, (size_t)-1,
				       &clus_count);
			resolve_path(&path_resolve_ctx, dir);
			exfat_debug("failed to check dentries: %s\n",
					path_resolve_ctx.local_path);
			ret = dir_errors;
		}

		list_del(&dir->list);
		inode_free_ancestors(dir);
	}
out:
	exfat_free_dir_list(exfat);
	return ret;
}

int main(int argc, const char *argv[])
{
	int optind = 1, err = 0;
	const char *blkdev_path, *out_path;
	struct pbr *bs;
	struct exfat_user_input ui;
	off_t last_sect;

	if (argc != 3)
		usage(argv[0]);

	print_level = EXFAT_ERROR;
	show_version();

	blkdev_path = argv[optind++];
	out_path = argv[optind++];

	memset(&ui, 0, sizeof(ui));
	snprintf(ui.dev_name, sizeof(ui.dev_name), "%s", blkdev_path);
	ui.writeable = false;

	if (exfat_get_blk_dev_info(&ui, &ei.bdev)) {
		exfat_err("failed to open %s\n", ui.dev_name);
		return EXIT_FAILURE;
	}

	err = read_boot_sect(&ei.bdev, &bs);
	if (err) {
		close(ei.bdev.dev_fd);
		return EXIT_FAILURE;
	}

	err = create_exfat2img(&ei, bs, out_path);
	if (err)
		return EXIT_FAILURE;

	err = dump_sectors(&ei, 0, le32_to_cpu(ei.exfat->bs->bsx.clu_offset));
	if (err) {
		exfat_err("failed to dump boot sectors, fat\n");
		goto out;
	}

	last_sect = le32_to_cpu(ei.exfat->bs->bsx.clu_offset) +
		(le32_to_cpu(ei.exfat->bs->bsx.clu_count) <<
		 ei.exfat->bs->bsx.sect_per_clus_bits) - 1;
	err = dump_sectors(&ei, last_sect, last_sect + 1);
	if (err) {
		exfat_err("failed to dump last sector\n");
		goto out;
	}

	err = dump_root(&ei);
	if (err) {
		exfat_err("failed to dump root\n");
		goto out;
	}

	dump_filesystem(&ei);

	err = fsync(ei.out_fd);
	if (err) {
		exfat_err("failed to fsync %s. %d\n", out_path, errno);
		goto out;
	}

	printf("%ld files found, %ld directories dumped, %llu kbytes written\n",
	       exfat_stat.file_count,
	       exfat_stat.dir_count,
	       (unsigned long long)DIV_ROUND_UP(exfat_stat.written_bytes, 1024));

out:
	free_exfat2img(&ei);
	return err == 0? EXIT_SUCCESS : EXIT_FAILURE;
}
