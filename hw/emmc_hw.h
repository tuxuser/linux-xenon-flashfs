/* emmc_hw.h - Xenon MMCX (Corona/Winchester eMMC) controller, PIO reads.
 *
 * Portable between userspace and the kernel module: the host supplies
 * register access through emmc_hw_ops. The register window handed to the ops
 * is the SFC block itself (physical 0xea00c000), offset 0 = SFCX_CONFIG.
 *
 * The KSB leaves the card selected (transfer state), so init only verifies
 * that and reads the sector count from EXT_CSD. Reads are 512-byte CMD17
 * transfers through the PIO FIFO using block addressing.
 */
#ifndef _EMMC_HW_H
#define _EMMC_HW_H

#include "flashfs_priv.h"

struct emmc_hw_ops {
    /* byte-swapped (device is big-endian, bus little) register access */
    uint32_t (*rd)(void *ctx, unsigned int off);
    void (*wr)(void *ctx, unsigned int off, uint32_t val);
    /* PIO FIFO word, native endianness (no swap) */
    uint32_t (*pio)(void *ctx);
    void (*pio_write)(void *ctx, uint32_t val);
    uint64_t (*now_ms)(void *ctx);
    void (*relax)(void *ctx);
    void (*log)(void *ctx, const char *msg, uint32_t a, uint32_t b);
};

typedef struct {
    const struct emmc_hw_ops *ops;
    void *ctx;
    uint32_t sectors;   /* from EXT_CSD */
    int ready;
} emmc_hw_t;

/* Returns 0 on success. */
int emmc_hw_init(emmc_hw_t *hw);

#define EMMC_CARD_RCA 0xffffu   /* relative address the KSB assigned */

/* EXT_CSD (CMD8), card status (CMD13, selected card) */
int emmc_hw_read_ext_csd(emmc_hw_t *hw, uint8_t out[512]);
int emmc_hw_card_status(emmc_hw_t *hw, uint32_t *status);
/* CID and CSD as the controller's response registers (RESP3..RESP0 order is
 * out[3]..out[0]); briefly de-selects the card, no other I/O may run. */
int emmc_hw_read_ident(emmc_hw_t *hw, uint32_t cid[4], uint32_t csd[4]);

/* Write one 512-byte sector (CMD24 via the PIO FIFO) and wait until the
 * card has finished programming. Caller must have write access approved. */
int emmc_hw_write_sector(emmc_hw_t *hw, uint32_t lba, const uint8_t data[512]);

/* Byte-granular write (read-modify-write for partial sectors). */
int emmc_hw_write(emmc_hw_t *hw, uint64_t offset, const void *buffer,
                  size_t length);

/* flashfs_image_read_fn-compatible: `context` is an emmc_hw_t. */
int emmc_hw_read(void *context, uint64_t offset, void *buffer, size_t length);

#endif
