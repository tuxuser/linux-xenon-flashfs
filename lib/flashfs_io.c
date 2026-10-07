#include "flashfs_priv.h"

int _flashfs_image_read(const flashfs_t *fs, uint64_t offset, void *buffer,
                        size_t length)
{
    return fs->image_read ? fs->image_read(fs->io_context, offset, buffer,
                                           length) : -1;
}

int _flashfs_block_spare(const flashfs_t *fs, uint32_t phys, uint8_t *sp)
{
    uint64_t stride = (uint64_t)fs->pages_block *
                      (fs->page_data + fs->page_spare);
    uint64_t offset = (uint64_t)phys * stride + fs->page_data;
    return _flashfs_image_read(fs, offset, sp, fs->page_spare);
}

int _flashfs_block_read(const flashfs_t *fs, uint32_t blk, uint8_t *out,
                        uint32_t len)
{
    uint32_t page, k;

    if (blk >= fs->nblocks || len > FLASHFS_BLOCK_LEN)
        return -1;
    if (fs->emmc)
        return _flashfs_image_read(fs, (uint64_t)blk * FLASHFS_BLOCK_LEN,
                                  out, len);

    uint32_t lba = blk / fs->lils_per_block;
    uint32_t phys = lba < fs->phys_blocks ? fs->lba2phys[lba] : NO_PHYS;
    if (phys == NO_PHYS)
        return -1;
    page = phys * fs->pages_block +
           (blk % fs->lils_per_block) * (FLASHFS_BLOCK_LEN / fs->page_data);
    for (k = 0; k * fs->page_data < len; k++) {
        uint8_t raw[FLASHFS_PAGE_LEN + 64u];
        uint32_t n = len - k * fs->page_data;
        uint64_t stride = fs->page_data + fs->page_spare;
        uint64_t offset = (uint64_t)(page + k) * stride;
        if (n > fs->page_data)
            n = fs->page_data;
        if (stride > sizeof(raw) ||
            _flashfs_image_read(fs, offset, raw, stride) != 0)
            return -1;
        memcpy(out + k * fs->page_data, raw, n);
    }
    return 0;
}

int _flashfs_read(const flashfs_t *fs, const flashfs_entry_t *e,
                  uint8_t *out, uint32_t off, uint32_t len)
{
    uint8_t *blk;
    uint32_t cur = e->block, skip = off, total = 0, guard = 0;

    if (off >= e->size)
        return 0;
    if (len > e->size - off)
        len = e->size - off;
    blk = malloc(FLASHFS_BLOCK_LEN);
    if (!blk)
        return -1;
    while (len && guard++ < fs->nblocks) {
        if (cur >= fs->nblocks)
            goto error;
        if (skip >= FLASHFS_BLOCK_LEN) {
            skip -= FLASHFS_BLOCK_LEN;
        } else {
            uint32_t n = FLASHFS_BLOCK_LEN - skip;
            if (n > len)
                n = len;
            if (_flashfs_block_read(fs, cur + fs->fs_offset, blk,
                                    FLASHFS_BLOCK_LEN) != 0)
                goto error;
            memcpy(out + total, blk + skip, n);
            skip = 0;
            total += n;
            len -= n;
        }
        cur = fs->blockmap[cur] & 0x7fffu;
        if (cur == FLASHFS_BLK_FREE || cur == FLASHFS_BLK_END)
            break;
    }
    free(blk);
    return (int)total;

error:
    free(blk);
    return -1;
}
