/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Copyright (C) 2021 Hyunchul Lee <hyc.lee@gmail.com>
 */
#ifndef _INODE_H
#define _INODE_H

#include "list.h"

typedef __u32 clus_t;

struct exfat_inode {
	struct exfat_inode	*parent;
	struct list_head	children;
	struct list_head	sibling;
	struct list_head	list;
	clus_t			first_clus;
	clus_t			last_lclus;
	clus_t			last_pclus;
	__u16			attr;
	uint64_t		size;
	bool			is_contiguous;
	__le16			name[0];	/* only for directory */
};

#define EXFAT_NAME_MAX			255
#define NAME_BUFFER_SIZE		((EXFAT_NAME_MAX+1)*2)

struct exfat {
	struct exfat_blk_dev	*blk_dev;
	struct pbr		*bs;
	char			volume_label[VOLUME_LABEL_BUFFER_SIZE];
	struct exfat_inode	*root;
	struct list_head	dir_list;
	clus_t			clus_count;
	unsigned int		clus_size;
	unsigned int		sect_size;
	char			*alloc_bitmap;
	char			*disk_bitmap;
	clus_t			disk_bitmap_clus;
	unsigned int		disk_bitmap_size;
};

struct path_resolve_ctx {
	struct exfat_inode	*ancestors[255];
	__le16			utf16_path[PATH_MAX + 2];
	char			local_path[PATH_MAX * MB_LEN_MAX + 1];
};

struct buffer_desc {
	__u32		p_clus;
	unsigned int	offset;
	char		*buffer;
	char		*dirty;
};

struct exfat *exfat_alloc_exfat(struct exfat_blk_dev *blk_dev, struct pbr *bs);
void exfat_free_exfat(struct exfat *exfat);

struct exfat_inode *alloc_exfat_inode(__u16 attr);
void free_exfat_inode(struct exfat_inode *node);

void inode_free_children(struct exfat_inode *dir, bool file_only);
void inode_free_file_children(struct exfat_inode *dir);
void inode_free_ancestors(struct exfat_inode *child);
void exfat_free_dir_list(struct exfat *exfat);

int resolve_path(struct path_resolve_ctx *ctx, struct exfat_inode *child);
int resolve_path_parent(struct path_resolve_ctx *ctx,
			struct exfat_inode *parent, struct exfat_inode *child);

struct buffer_desc *exfat_alloc_buffer(int count,
				unsigned int clu_size, unsigned int sect_size);
void exfat_free_buffer(struct buffer_desc *bd, int count);
#endif
