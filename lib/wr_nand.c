/* wr_nand.c - writer backend for raw small-block NAND: a logical block is one
 * physical block (fs->lba2phys), data is erased + reprogrammed in place with its
 * spare kept, and the root is committed as a NEW block with sequence + 1. */
#include "flashfs_wr_priv.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

static uint32_t nand_phys(const flashfs_wr_t *wr, uint32_t lba)
{
    return lba < wr->fs->phys_blocks ? wr->fs->lba2phys[lba] : NO_PHYS;
}

static int nand_read_raw(flashfs_wr_t *wr, uint32_t phys, uint8_t *raw)
{
    uint32_t ppb = wr->fs->pages_block;

    return _flashfs_image_read(wr->fs, (uint64_t)phys * ppb * REC, raw,
                               (size_t)ppb * REC);
}

/* spare of a fresh data block: lba, seq 0, good-block marker, type 0 (as the OS
 * writes its plain data blocks); the ECC bytes are the backend's job */
static void spare_data(uint8_t sp[16], uint32_t lba)
{
    memset(sp, 0, 16);
    sp[0] = (uint8_t)lba;
    sp[1] = (uint8_t)((lba >> 8) & 0xf);
    sp[5] = 0xff;
    sp[13] = sp[14] = sp[15] = 0xff;
}

static int nand_program(flashfs_wr_t *wr, uint32_t phys, const uint8_t *raw,
                        const uint8_t *order, uint32_t n)
{
    uint32_t i, ppb = wr->fs->pages_block;
    int ret;

    for (i = 0; i < n; i++) {
        ret = wr->write(wr->wctx, ((uint64_t)phys * ppb + order[i]) * REC,
                        raw + (size_t)order[i] * REC, REC);
        if (ret)
            return ret;
    }
    return 0;
}

static const uint8_t seq_order[32] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };

/* program data into a block that is erased (and unclaimed) */
static int nand_fresh(flashfs_wr_t *wr, uint32_t b, const uint8_t *buf)
{
    uint32_t p;
    int ret;

    for (p = 0; p < wr->fs->pages_block; p++) {
        memcpy(wr->raw + (size_t)p * REC, buf + (size_t)p * FLASHFS_PAGE_LEN,
               FLASHFS_PAGE_LEN);
        spare_data(wr->raw + (size_t)p * REC + FLASHFS_PAGE_LEN, b);
    }
    ret = nand_program(wr, b, wr->raw, seq_order, wr->fs->pages_block);
    if (!ret)
        wr->fs->lba2phys[b] = (uint16_t)b;
    return ret;
}

/* return a block to the erased state */
static int nand_wipe(flashfs_wr_t *wr, uint32_t b)
{
    uint32_t phys = nand_phys(wr, b);
    int ret;

    if (phys == NO_PHYS)
        return 0;
    ret = wr->erase(wr->wctx, phys);
    if (!ret)
        wr->fs->lba2phys[b] = NO_PHYS;
    return ret;
}

/* Overwrite bytes of a block in place: erase + reprogram of the SAME physical
 * block with its spare bytes kept (nothing is copied elsewhere). */
static int nand_patch(flashfs_wr_t *wr, uint32_t b, uint32_t off,
                      const uint8_t *data, uint32_t len)
{
    uint32_t phys = nand_phys(wr, b), i, differs = 0;
    int ret;

    if (phys == NO_PHYS || off + len > B)
        return -EIO;
    ret = nand_read_raw(wr, phys, wr->raw);
    if (ret)
        return ret;
    for (i = 0; i < len; i++) {
        uint32_t o = off + i;
        uint8_t *d = wr->raw + (size_t)(o / FLASHFS_PAGE_LEN) * REC +
                     o % FLASHFS_PAGE_LEN;

        if (*d != data[i]) {
            *d = data[i];
            differs = 1;
        }
    }
    if (!differs)
        return 0;
    ret = wr->erase(wr->wctx, phys);
    if (ret)
        return ret;
    return nand_program(wr, phys, wr->raw, seq_order, wr->fs->pages_block);
}

static int nand_is_erased(flashfs_wr_t *wr, uint32_t b)
{
    uint32_t i, n = wr->fs->pages_block * REC;

    if (b >= wr->fs->phys_blocks || nand_phys(wr, b) != NO_PHYS ||
        nand_read_raw(wr, b, wr->raw))
        return 0;
    for (i = 0; i < n; i++)
        if (wr->raw[i] != 0xff)
            return 0;
    return 1;
}

static int nand_reserved(const flashfs_wr_t *wr, uint32_t b)
{
    (void)wr, (void)b;
    return 0;
}

/* NAND: discover the newest root (highest spare sequence with a sane table) and
 * the fallback before it, exactly as the reader does. */
static int nand_find_roots(flashfs_wr_t *wr)
{
    flashfs_t *fs = wr->fs;
    fs_candidate_t *c = malloc(MAX_CANDIDATES * sizeof(*c));
    uint32_t n, k, best, best_seq = 0, fb = 0xffffu, fb_seq = 0;

    if (!c)
        return -ENOMEM;
    best = _flashfs_nand_find_root(fs, c, &n);
    if (best == 0xffffu) {
        free(c);
        return -EILSEQ;
    }
    for (k = 0; k < n; k++)
        if (c[k].lil == best)
            best_seq = c[k].seq;
    for (k = 0; k < n; k++)
        if (c[k].seq < best_seq && c[k].seq >= fb_seq &&
            _flashfs_table_sane(fs, c[k].lil)) {
            fb = c[k].lil;
            fb_seq = c[k].seq;
        }
    wr->roots = c;
    wr->nroots = n;
    wr->root_blk = (uint16_t)best;
    wr->gen = best_seq;
    wr->fb_blk = (uint16_t)fb;
    return 0;
}

static int nand_open(flashfs_wr_t *wr)
{
    flashfs_t *fs = wr->fs;

    if (fs->spare != SPARE_SB || fs->lils_per_block != 1 ||
        fs->page_data != FLASHFS_PAGE_LEN || fs->page_spare != 16 || !wr->erase)
        return -EOPNOTSUPP;
    wr->raw = malloc((size_t)fs->pages_block * REC);
    if (!wr->raw)
        return -ENOMEM;
    return nand_find_roots(wr);
}

/* NAND: the new root is a new block with sequence + 1. Page order makes a torn
 * write harmless: the reader needs the spare (page 0) and a sane first entries
 * page (page 1), which is written last. */
static int nand_commit_root(flashfs_wr_t *wr, uint32_t r)
{
    uint32_t p, ppb = wr->fs->pages_block, seq = wr->gen + 1;
    uint8_t order[32];
    uint8_t *old = wr->raw;
    int ret;

    ret = nand_read_raw(wr, nand_phys(wr, wr->root_blk), old);
    if (ret)
        return ret;
    /* new raw records: data from the working root, spare = the old root's
     * (every byte we do not own is copied) with lba and sequence replaced */
    {
        uint8_t *nw = malloc((size_t)ppb * REC);

        if (!nw)
            return -ENOMEM;
        for (p = 0; p < ppb; p++) {
            uint8_t *sp = nw + (size_t)p * REC + FLASHFS_PAGE_LEN;

            memcpy(nw + (size_t)p * REC, wr->root + (size_t)p * FLASHFS_PAGE_LEN,
                   FLASHFS_PAGE_LEN);
            memcpy(sp, old + (size_t)p * REC + FLASHFS_PAGE_LEN, 16);
            sp[0] = (uint8_t)r;
            sp[1] = (uint8_t)((sp[1] & 0xf0) | ((r >> 8) & 0xf));
            sp[2] = (uint8_t)seq;
            sp[3] = (uint8_t)(seq >> 8);
            sp[4] = (uint8_t)(seq >> 16);
            sp[6] = (uint8_t)(seq >> 24);
            sp[13] = sp[14] = sp[15] = 0xff;
        }
        memcpy(old, nw, (size_t)ppb * REC);
        free(nw);
    }
    for (p = 0; p + 1 < ppb; p++)
        order[p] = (uint8_t)(p + 2 < ppb ? p + 2 : 0);
    order[ppb - 2] = 0;
    order[ppb - 1] = 1;
    ret = nand_program(wr, r, old, order, ppb);
    wr->nonerased[r] = 1;
    wr->fs->lba2phys[r] = (uint16_t)r;
    if (ret)
        goto undo;
    if (rd_block(wr, r, wr->tmp) || memcmp(wr->tmp, wr->root, B)) {
        ret = -EIO;
        goto undo;
    }
    return 0;
undo:
    /* never leave a half-written newest root behind */
    if (!wr->erase(wr->wctx, r)) {
        wr->fs->lba2phys[r] = NO_PHYS;
        wr->nonerased[r] = 0;
    }
    return ret;
}


/* roots older than the previous one are erased (NAND has only ~20 spare
 * blocks); the previous root stays as the fallback */
static void nand_committed(flashfs_wr_t *wr, uint32_t r)
{
    uint32_t k, keep = 0;

    for (k = 0; k < wr->nroots; k++) {
        fs_candidate_t *c = &wr->roots[k];

        if (c->seq < wr->gen) {
            if (!nand_wipe(wr, c->lil))
                wr->nonerased[c->lil] = 0;
            continue;
        }
        wr->roots[keep++] = *c;
    }
    wr->nroots = keep;
    if (wr->nroots < MAX_CANDIDATES) {
        wr->roots[wr->nroots].lil = (uint16_t)r;
        wr->roots[wr->nroots].seq = wr->gen + 1;
        wr->nroots++;
    }
    wr->gen++;
}

const struct flashfs_wr_backend flashfs_wr_nand = {
    .gen_name = "sequence",
    .open = nand_open,
    .fresh = nand_fresh,
    .wipe = nand_wipe,
    .patch = nand_patch,
    .is_erased = nand_is_erased,
    .reserved = nand_reserved,
    .commit_root = nand_commit_root,
    .committed = nand_committed,
};
