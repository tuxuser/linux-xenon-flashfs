/* flashfs_wr_priv.h - writer internals shared by the common core and the
 * eMMC / NAND backends */
#ifndef _FLASHFS_WR_PRIV_H
#define _FLASHFS_WR_PRIV_H

#include "flashfs_write.h"

#define B       FLASHFS_BLOCK_LEN
#define NAMELEN FLASHFS_NAME_LEN
#define REC     (FLASHFS_PAGE_LEN + 16u)   /* raw NAND record: page + spare */

/* How the two storage kinds differ. The common core (flashfs_write.c) owns the
 * file table logic and calls these for everything that touches the media. */
struct flashfs_wr_backend {
    const char *gen_name;                  /* what wr->gen counts */
    /* find the newest root: sets root_blk, gen, fb_blk (0xffff if none) */
    int (*open)(flashfs_wr_t *wr);
    int (*fresh)(flashfs_wr_t *wr, uint32_t b, const uint8_t *buf);
    int (*wipe)(flashfs_wr_t *wr, uint32_t b);
    int (*patch)(flashfs_wr_t *wr, uint32_t b, uint32_t off, const uint8_t *data,
                 uint32_t len);
    int (*is_erased)(flashfs_wr_t *wr, uint32_t b);
    int (*reserved)(const flashfs_wr_t *wr, uint32_t b);
    /* make wr->root durable as block r and point the media at it */
    int (*commit_root)(flashfs_wr_t *wr, uint32_t r);
    /* bookkeeping once the generic reclaim is done */
    void (*committed)(flashfs_wr_t *wr, uint32_t r);
};

extern const struct flashfs_wr_backend flashfs_wr_emmc, flashfs_wr_nand;

static inline int rd_block(flashfs_wr_t *wr, uint32_t b, uint8_t *buf)
{
    return _flashfs_block_read(wr->fs, b, buf, B);
}

static inline uint16_t map_get(const uint8_t *r, uint32_t i)
{
    return rd16be(r + (size_t)(i / 256) * 2 * FLASHFS_PAGE_LEN + (i % 256) * 2);
}

static inline void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

#endif
