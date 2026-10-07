/* flashfs_mount.c - open/close a flashfs image: geometry, root discovery, parse */
#include "flashfs_priv.h"

/* eMMC keeps two root pointers; the higher version wins. */
static uint16_t emmc_find_root(const flashfs_t *fs)
{
    uint8_t slot[0x20];
    uint32_t best_ver = 0;
    uint16_t best = NO_PHYS;
    unsigned int b;

    for (b = 0; b < 2; b++) {
        uint32_t ver;
        uint16_t blk;

        if (_flashfs_image_read(fs, EMMC_ROOT_ADDR + (uint64_t)b * FLASHFS_BLOCK_LEN,
                                slot, sizeof(slot)))
            continue;
        ver = rd32be(slot + 0x18);
        blk = rd16be(slot + 0x1c);
        if (ver > best_ver && blk < fs->nblocks) {
            best_ver = ver;
            best = blk;
        }
    }
    return best;
}

static int set_geometry(flashfs_t *fs)
{
    if (!fs->emmc) {
        if (fs->page_data != FLASHFS_PAGE_LEN || !fs->page_spare ||
            fs->page_spare > 64 || !fs->pages_block || !fs->phys_blocks ||
            (uint64_t)fs->pages_block * fs->page_data % FLASHFS_BLOCK_LEN)
            return -EINVAL;
        fs->lils_per_block = fs->pages_block * fs->page_data / FLASHFS_BLOCK_LEN;
        fs->nblocks = fs->phys_blocks * fs->lils_per_block;
    }
    return (!fs->nblocks || fs->nblocks > 0xffff) ? -EINVAL : 0;
}

int flashfs_open(flashfs_t *fs)
{
    fs_candidate_t cands[MAX_CANDIDATES];
    uint32_t n, i;
    uint16_t root;
    int ret = set_geometry(fs);

    if (ret)
        return ret;
    fs->blockmap = malloc((size_t)fs->nblocks * sizeof(*fs->blockmap));
    fs->entries = calloc(fs->nblocks, sizeof(*fs->entries));
    if (!fs->emmc)
        fs->lba2phys = malloc((size_t)fs->phys_blocks * sizeof(*fs->lba2phys));
    if (!fs->blockmap || !fs->entries || (!fs->emmc && !fs->lba2phys)) {
        flashfs_close(fs);
        return -ENOMEM;
    }
    memset(fs->blockmap, 0xfe, (size_t)fs->nblocks * sizeof(*fs->blockmap));
    for (i = 0; !fs->emmc && i < fs->phys_blocks; i++)
        fs->lba2phys[i] = NO_PHYS;

    root = fs->emmc ? emmc_find_root(fs) : _flashfs_nand_find_root(fs, cands, &n);
    if (root >= fs->nblocks || _flashfs_mount_root(fs, root) || !fs->nentries) {
        flashfs_close(fs);
        return -EINVAL;
    }
    return 0;
}

void flashfs_close(flashfs_t *fs)
{
    free(fs->lba2phys);
    free(fs->blockmap);
    free(fs->entries);
    fs->lba2phys = NULL;
    fs->blockmap = NULL;
    fs->entries = NULL;
}
