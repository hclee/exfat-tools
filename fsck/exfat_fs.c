// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2021 Hyunchul Lee <hyc.lee@gmail.com>
 */

#include <errno.h>

#include "exfat_ondisk.h"
#include "libexfat.h"

#include "inode.h"
#include "exfat_fs.h"

int exfat_o2c(struct exfat *exfat, off_t device_offset,
	      unsigned int *clu, unsigned int *offset)
{
	off_t heap_offset;

	heap_offset = exfat_s2o(exfat, le32_to_cpu(exfat->bs->bsx.clu_offset));
	if (device_offset < heap_offset)
		return -ERANGE;

	*clu = (unsigned int)((device_offset - heap_offset) /
			      exfat->clus_size) + EXFAT_FIRST_CLUSTER;
	if (!heap_clus(exfat, *clu))
		return -ERANGE;
	*offset = (device_offset - heap_offset) % exfat->clus_size;
	return 0;
}

void exfat_bitmap_set_range(struct exfat *exfat, char *bitmap,
			clus_t start_clus, clus_t count)
{
	clus_t clus;

	if (!heap_clus(exfat, start_clus) ||
		!heap_clus(exfat, start_clus + count))
		return;

	clus = start_clus;
	while (clus < start_clus + count) {
		EXFAT_BITMAP_SET(bitmap,
				clus - EXFAT_FIRST_CLUSTER);
		clus++;
	}
}

int get_next_clus(struct exfat *exfat, struct exfat_inode *node,
				clus_t clus, clus_t *next)
{
	off_t offset;

	*next = EXFAT_EOF_CLUSTER;

	if (!heap_clus(exfat, clus))
		return -EINVAL;

	if (node->is_contiguous) {
		*next = clus + 1;
		return 0;
	}

	offset = (off_t)le32_to_cpu(exfat->bs->bsx.fat_offset) <<
				exfat->bs->bsx.sect_size_bits;
	offset += sizeof(clus_t) * clus;

	if (exfat_read(exfat->blk_dev->dev_fd, next, sizeof(*next), offset)
			!= sizeof(*next))
		return -EIO;
	*next = le32_to_cpu(*next);
	return 0;
}

int set_fat(struct exfat *exfat, clus_t clus, clus_t next_clus)
{
	off_t offset;

	offset = le32_to_cpu(exfat->bs->bsx.fat_offset) <<
		exfat->bs->bsx.sect_size_bits;
	offset += sizeof(clus_t) * clus;

	if (exfat_write(exfat->blk_dev->dev_fd, &next_clus, sizeof(next_clus),
			offset) != sizeof(next_clus))
		return -EIO;
	return 0;
}
