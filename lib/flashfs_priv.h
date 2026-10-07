/* flashfs_priv.h - shared on-disk parser internals */
#ifndef _FLASHFS_PRIV_H
#define _FLASHFS_PRIV_H

#ifdef __KERNEL__
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
typedef u8 uint8_t;
typedef u16 uint16_t;
typedef u32 uint32_t;
typedef u64 uint64_t;
#define malloc(n) kmalloc((n), GFP_KERNEL)
#define calloc(n, s) kcalloc((n), (s), GFP_KERNEL)
#define free(p) kfree(p)
#else
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#endif

#define FLASHFS_BLOCK_LEN   0x4000u
#define FLASHFS_PAGE_LEN    0x200u
#define FLASHFS_ENTRY_LEN   0x20u
#define FLASHFS_NAME_LEN    0x16u
#define FLASHFS_BLK_FREE    0x1ffeu
#define FLASHFS_BLK_END     0x1fffu
#define FLASHFS_BLK_RSRVD   0x1ffbu
#define EMMC_FLASH_SIZE     0x3000000u   /* logical flash: first 48 MiB */
#define EMMC_ROOT_ADDR      0x2FE8000u   /* two root slots, higher version wins */
#define HW_WINDOW           0x4000000u
#define MAX_CANDIDATES      64
#define NO_PHYS             0xFFFFu

#define SPARE_SB            0
#define SPARE_JASPER        1
#define SPARE_BB            2

typedef struct {
    char name[FLASHFS_NAME_LEN + 1];
    uint16_t block;
    uint32_t size;
    uint32_t timestamp;
} flashfs_entry_t;

typedef int (*flashfs_image_read_fn)(void *context, uint64_t offset,
                                     void *buffer, size_t length);

typedef struct {
    int emmc;
    void *io_context;
    flashfs_image_read_fn image_read;
    uint32_t nblocks;
    uint16_t fs_offset;
    uint16_t *blockmap;
    flashfs_entry_t *entries;
    uint32_t nentries;
    uint32_t page_data, page_spare, pages_block;
    int spare;
    uint32_t lils_per_block, phys_blocks;
    uint16_t *lba2phys;
} flashfs_t;

typedef struct {
    uint16_t lil;
    uint32_t seq;
} fs_candidate_t;

static inline uint16_t rd16be(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t rd32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int _flashfs_image_read(const flashfs_t *fs, uint64_t offset, void *buffer,
                        size_t length);
int _flashfs_block_spare(const flashfs_t *fs, uint32_t phys, uint8_t *sp);
int _flashfs_block_read(const flashfs_t *fs, uint32_t blk, uint8_t *out,
                        uint32_t len);
int _flashfs_read(const flashfs_t *fs, const flashfs_entry_t *e,
                  uint8_t *out, uint32_t off, uint32_t len);
uint16_t _flashfs_nand_find_root(flashfs_t *fs, fs_candidate_t *cands,
                                 uint32_t *ncands);
int _flashfs_table_sane(const flashfs_t *fs, uint32_t blk);
int _flashfs_mount_root(flashfs_t *fs, uint16_t root);

/* Caller fills io_context, image_read, emmc and either nblocks (eMMC) or
 * page_data/page_spare/pages_block/phys_blocks/spare (NAND). */
int flashfs_open(flashfs_t *fs);
void flashfs_close(flashfs_t *fs);

#endif
