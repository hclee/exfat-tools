/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Copyright (C) 2021 Hyunchul Lee <hyc.lee@gmail.com>
 */
#ifndef _EXFAT_FS_H
#define _EXFAT_FS_H

struct exfat;
struct exfat_de_iter;

struct exfat_lookup_filter {
	struct {
		uint8_t		type;
		/* return 0 if matched, return 1 if not matched,
		 * otherwise return errno
		 */
		int		(*filter)(struct exfat_de_iter *iter,
					void *param, int *dentry_count);
		void		*param;
	} in;
	struct {
		struct exfat_dentry	*dentry_set;
		int			dentry_count;
		/* device offset where the dentry_set locates, or
		 * the empty slot locates or EOF if not found.
		 */
		off_t			dentry_d_offset;
	} out;
};

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

static inline bool exfat_bitmap_get(char *bmap, clus_t c)
{
	clus_t cc = c - EXFAT_FIRST_CLUSTER;
	return ((bitmap_t *)(bmap))[BIT_ENTRY(cc)] & BIT_MASK(cc);
}

static inline void exfat_bitmap_set(char *bmap, clus_t c)
{
	clus_t cc = c - EXFAT_FIRST_CLUSTER;
	(((bitmap_t *)(bmap))[BIT_ENTRY(cc)] |= BIT_MASK(cc));
}

void exfat_bitmap_set_range(struct exfat *exfat, char *bitmap,
			    clus_t start_clus, clus_t count);

#define EXFAT_CLUSTER_SIZE(pbr) (1 << ((pbr)->bsx.sect_size_bits +	\
					(pbr)->bsx.sect_per_clus_bits))
#define EXFAT_SECTOR_SIZE(pbr) (1 << (pbr)->bsx.sect_size_bits)

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

int exfat_o2c(struct exfat *exfat, off_t device_offset,
	      unsigned int *clu, unsigned int *offset);

static inline bool heap_clus(struct exfat *exfat, clus_t clus)
{
	return clus >= EXFAT_FIRST_CLUSTER &&
		(clus - EXFAT_FIRST_CLUSTER) < exfat->clus_count;
}

int get_next_clus(struct exfat *exfat, clus_t clus, clus_t *next);
int get_inode_next_clus(struct exfat *exfat, struct exfat_inode *node,
				clus_t clus, clus_t *next);
int set_fat(struct exfat *exfat, clus_t clus, clus_t next_clus);

/* lookup.c */
int exfat_lookup_dentry_set(struct exfat *exfat, struct exfat_inode *parent,
		struct exfat_lookup_filter *filter);
int exfat_lookup_file(struct exfat *exfat, struct exfat_inode *parent,
		const char *name, struct exfat_lookup_filter *filter_out);

/* create.c */
int exfat_create_file(struct exfat *exfat, struct exfat_inode *parent,
		      const char *name, unsigned short attr);

#endif
