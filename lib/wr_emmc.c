/* wr_emmc.c - writer backend for the flat eMMC image: blocks are written in
 * place, the root is committed by rewriting the OLDER of two 512-byte slots
 * with version + 1 and a SHA-1 (see flashfs_write.h). */
#include "flashfs_wr_priv.h"
#include "sha1.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

static int slot_read(flashfs_wr_t *wr, int s, uint8_t hdr[FLASHFS_SLOT_LEN],
                     uint32_t *ver, uint32_t *root, int *hash_ok)
{
    uint8_t h[20];

    if (_flashfs_image_read(wr->fs,
                            EMMC_ROOT_ADDR + (uint64_t)s * B, hdr,
                            FLASHFS_SLOT_LEN))
        return -EIO;
    flashfs_sha1(hdr + FLASHFS_SLOT_HASH_OFF,
                 FLASHFS_SLOT_LEN - FLASHFS_SLOT_HASH_OFF, h);
    *hash_ok = !memcmp(h, hdr, 20);
    *ver = rd32be(hdr + FLASHFS_SLOT_VER_OFF);
    *root = rd16be(hdr + FLASHFS_SLOT_ROOT_OFF);
    return 0;
}

static int emmc_open(flashfs_wr_t *wr)
{
    uint8_t hdr[2][FLASHFS_SLOT_LEN];
    uint32_t ver[2], root[2];
    int ok[2], s, best, ret;

    for (s = 0; s < 2; s++) {
        ret = slot_read(wr, s, hdr[s], &ver[s], &root[s], &ok[s]);
        if (ret)
            return ret;
    }
    /* the newest slot must be valid, and the one the reader mounted */
    best = ver[1] > ver[0];
    if (!ok[best] || root[best] >= wr->fs->nblocks)
        return -EILSEQ;
    wr->newest_slot = best;
    wr->gen = ver[best];
    wr->root_blk = (uint16_t)root[best];
    memcpy(wr->slot, hdr[best], FLASHFS_SLOT_LEN);
    if (ok[!best] && root[!best] < wr->fs->nblocks)
        wr->fb_blk = (uint16_t)root[!best];
    return 0;
}

static int emmc_fresh(flashfs_wr_t *wr, uint32_t b, const uint8_t *buf)
{
    return wr->write(wr->wctx, (uint64_t)b * B, buf, B);
}

static int emmc_wipe(flashfs_wr_t *wr, uint32_t b)
{
    memset(wr->tmp, 0xff, B);
    return wr->write(wr->wctx, (uint64_t)b * B, wr->tmp, B);
}

static int emmc_patch(flashfs_wr_t *wr, uint32_t b, uint32_t off,
                      const uint8_t *data, uint32_t len)
{
    return wr->write(wr->wctx, (uint64_t)b * B + off, data, len);
}

static int emmc_is_erased(flashfs_wr_t *wr, uint32_t b)
{
    uint32_t i;

    if (rd_block(wr, b, wr->tmp))
        return 0;
    for (i = 0; i < B; i++)
        if (wr->tmp[i] != 0xff)
            return 0;
    return 1;
}

/* the slot header holds block numbers whose meaning is not known */
static int emmc_reserved(const flashfs_wr_t *wr, uint32_t b)
{
    uint32_t o;

    for (o = FLASHFS_SLOT_ROOT_OFF; o + 2 <= 0x44; o += 2)
        if (rd16be(wr->slot + o) == b)
            return 1;
    return 0;
}

static int emmc_commit_root(flashfs_wr_t *wr, uint32_t r)
{
    uint8_t *slot = wr->next_slot, h[20];
    int ret = emmc_fresh(wr, r, wr->root);

    wr->nonerased[r] = 1;
    if (ret)
        return ret;
    if (rd_block(wr, r, wr->tmp) || memcmp(wr->tmp, wr->root, B))
        return -EIO;                /* new root did not read back */

    memcpy(slot, wr->slot, FLASHFS_SLOT_LEN);
    put32(slot + FLASHFS_SLOT_VER_OFF, wr->gen + 1);
    put16(slot + FLASHFS_SLOT_ROOT_OFF, (uint16_t)r);
    /* every other header field (incl. the block pointer at 0x40) is copied
     * unchanged: its meaning is unknown, and a copy is a known-valid value */
    flashfs_sha1(slot + FLASHFS_SLOT_HASH_OFF,
                 FLASHFS_SLOT_LEN - FLASHFS_SLOT_HASH_OFF, h);
    memcpy(slot, h, 20);
    wr->next_target = !wr->newest_slot;     /* overwrite the OLDER slot only */
    ret = wr->write(wr->wctx, EMMC_ROOT_ADDR + (uint64_t)wr->next_target * B, slot,
                    FLASHFS_SLOT_LEN);
    if (ret)
        return ret;
    if (_flashfs_image_read(wr->fs, EMMC_ROOT_ADDR + (uint64_t)wr->next_target * B,
                            wr->tmp, FLASHFS_SLOT_LEN) ||
        memcmp(wr->tmp, slot, FLASHFS_SLOT_LEN))
        return -EIO;
    return 0;
}

/* superseded root blocks are deliberately NOT wiped (OS-written, one leaked
 * block per commit) */
static void emmc_committed(flashfs_wr_t *wr, uint32_t r)
{
    (void)r;
    wr->gen++;
    wr->newest_slot = wr->next_target;
    memcpy(wr->slot, wr->next_slot, FLASHFS_SLOT_LEN);
}

const struct flashfs_wr_backend flashfs_wr_emmc = {
    .gen_name = "version",
    .open = emmc_open,
    .fresh = emmc_fresh,
    .wipe = emmc_wipe,
    .patch = emmc_patch,
    .is_erased = emmc_is_erased,
    .reserved = emmc_reserved,
    .commit_root = emmc_commit_root,
    .committed = emmc_committed,
};
