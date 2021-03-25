/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Copyright (C) 2020 Hyunchul Lee <hyc.lee@gmail.com>
 */
#ifndef _FSCK_H
#define _FSCK_H

enum fsck_ui_options {
	FSCK_OPTS_REPAIR_ASK	= 0x01,
	FSCK_OPTS_REPAIR_YES	= 0x02,
	FSCK_OPTS_REPAIR_NO	= 0x04,
	FSCK_OPTS_REPAIR_AUTO	= 0x08,
	FSCK_OPTS_REPAIR_WRITE	= 0x0b,
	FSCK_OPTS_REPAIR_ALL	= 0x0f,
};

struct exfat;
struct exfat_inode;

struct exfat_fsck {
	struct exfat		*exfat;
	struct exfat_de_iter	de_iter;
	struct buffer_desc	*buffer_desc;	/* cluster * 2 */
	enum fsck_ui_options	options;
	bool			dirty:1;
	bool			dirty_fat:1;
};

off_t exfat_c2o(struct exfat *exfat, unsigned int clus);
int get_next_clus(struct exfat *exfat, struct exfat_inode *node,
				clus_t clus, clus_t *next);

#endif
