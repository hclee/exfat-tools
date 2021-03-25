/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Copyright (C) 2021 Hyunchul Lee <hyc.lee@gmail.com>
 */
#ifndef _EXFAT_FS_H
#define _EXFAT_FS_H

struct exfat;

#define EXFAT_CLUSTER_SIZE(pbr) (1 << ((pbr)->bsx.sect_size_bits +	\
					(pbr)->bsx.sect_per_clus_bits))
#define EXFAT_SECTOR_SIZE(pbr) (1 << (pbr)->bsx.sect_size_bits)

#ifdef WORDS_BIGENDIAN
typedef __u8	bitmap_t;
#else
typedef __u32	bitmap_t;
#endif

#define BITS_PER	(sizeof(bitmap_t) * 8)
#define BIT_MASK(__c)	(1 << ((__c) % BITS_PER))
#define BIT_ENTRY(__c)	((__c) / BITS_PER)

#define EXFAT_BITMAP_SIZE(__c_count)	\
	(DIV_ROUND_UP(__c_count, BITS_PER) * sizeof(bitmap_t))
#define EXFAT_BITMAP_GET(__bmap, __c)	\
			(((bitmap_t *)(__bmap))[BIT_ENTRY(__c)] & BIT_MASK(__c))
#define EXFAT_BITMAP_SET(__bmap, __c)	\
			(((bitmap_t *)(__bmap))[BIT_ENTRY(__c)] |= \
			 BIT_MASK(__c))

void exfat_bitmap_set_range(struct exfat *exfat, clus_t start_clus,
			    clus_t count);

static inline off_t exfat_s2o(struct exfat *exfat, off_t sect)
{
	return sect << exfat->bs->bsx.sect_size_bits;
}

static inline off_t exfat_c2o(struct exfat *exfat, unsigned int clus)
{
	if (clus < EXFAT_FIRST_CLUSTER)
		return ~0L;

	return exfat_s2o(exfat, le32_to_cpu(exfat->bs->bsx.clu_offset) +
				((off_t)(clus - EXFAT_FIRST_CLUSTER) <<
				 exfat->bs->bsx.sect_per_clus_bits));
}

static inline bool heap_clus(struct exfat *exfat, clus_t clus)
{
	return clus >= EXFAT_FIRST_CLUSTER &&
		(clus - EXFAT_FIRST_CLUSTER) < exfat->clus_count;
}

int get_next_clus(struct exfat *exfat, struct exfat_inode *node,
				clus_t clus, clus_t *next);
int set_fat(struct exfat *exfat, clus_t clus, clus_t next_clus);


#endif
