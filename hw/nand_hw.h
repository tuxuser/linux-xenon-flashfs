/* nand_hw.h - Xenon SFC raw NAND access (read / program / erase), portable
 * between userspace tests and the kernel module.
 *
 * The SFC exposes a page buffer; "physical" pages are 512 data + 16 spare bytes.
 * Reads can be done for every layout. Programming and erasing are only supported
 * for the small-block (pre-Jasper) layout: the ECC (a 26-bit code over the 512
 * data bytes + spare[0..11], stored in spare[12..15]) is generated in SOFTWARE
 * here, exactly like libxenon does, and has only been verified against real
 * small-block data (see tests/nand_ecc_check.py).
 *
 * Safety: every program is verified by a read-back; a block's bad-block marker
 * is never touched by the page API except by an explicit erase+program by the
 * caller (the MTD layer refuses bad blocks).
 */
#ifndef _NAND_HW_H
#define _NAND_HW_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/errno.h>
#else
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#endif

#define NAND_RAW_PAGE   528u
#define NAND_DATA_BYTES 512u

struct nand_hw_ops {
    /* register value access (host does the byte swap) */
    uint32_t (*rd)(void *ctx, unsigned int off);
    void (*wr)(void *ctx, unsigned int off, uint32_t val);
    uint64_t (*now_ms)(void *ctx);
    void (*relax)(void *ctx);
};

typedef struct {
    const struct nand_hw_ops *ops;
    void *ctx;
    uint32_t pages_per_block;
    uint32_t blocks;
    int writable;                /* small-block layout only */
    uint32_t last_status;        /* SFCX_STATUS after the last operation */
} nand_hw_t;

/* 26-bit page ECC, stored little-endian in spare[12..15] (bits 0..5 of spare[12]
 * are not part of the code and are left to the caller). */
void nand_hw_ecc(const uint8_t raw[NAND_RAW_PAGE], uint8_t out[4]);
int nand_hw_ecc_ok(const uint8_t raw[NAND_RAW_PAGE]);

int nand_hw_read_page(nand_hw_t *hw, uint32_t page, uint8_t raw[NAND_RAW_PAGE]);
/* Program a page: spare[12..15] is replaced by the computed ECC (keeping the
 * low 6 bits of spare[12] from `raw`), then verified by reading it back. */
int nand_hw_write_page(nand_hw_t *hw, uint32_t page,
                       const uint8_t raw[NAND_RAW_PAGE]);
int nand_hw_erase_block(nand_hw_t *hw, uint32_t block);
/* 1 if the first page's spare marks the block bad (small block: byte 5 != 0xff) */
int nand_hw_block_marked_bad(nand_hw_t *hw, uint32_t block);

#endif
