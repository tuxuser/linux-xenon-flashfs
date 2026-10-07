/* flashfs_write.c - see flashfs_write.h for the commit protocol */
#include "flashfs_wr_priv.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#include <stdio.h>
#include <strings.h>
#endif

static void map_set(uint8_t *r, uint32_t i, uint16_t v)
{
    uint8_t *p = r + (size_t)(i / 256) * 2 * FLASHFS_PAGE_LEN + (i % 256) * 2;

    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint8_t *ent(const uint8_t *r, uint32_t i)
{
    return (uint8_t *)r + ((size_t)(i / 16) * 2 + 1) * FLASHFS_PAGE_LEN +
           (i % 16) * FLASHFS_ENTRY_LEN;
}

/* Same end-of-table rule as the parser (root_parse). */
static uint32_t ent_count(const uint8_t *r)
{
    uint32_t i;

    for (i = 0; i < FLASHFS_MAX_ENTRIES; i++) {
        const uint8_t *e = ent(r, i);

        if (e[0] == 0 || rd16be(e + NAMELEN) == 0xffffu ||
            rd32be(e + NAMELEN + 2) == 0)
            return i;
    }
    return FLASHFS_MAX_ENTRIES;
}

/* name as the parser shows it (leading 0x05 -> '_'), NUL terminated */
static void disp_name(const uint8_t *e, char out[NAMELEN + 1])
{
    memcpy(out, e, NAMELEN);
    out[NAMELEN] = 0;
    if (out[0] == 5)
        out[0] = '_';
}

static int name_ok(const char *name)
{
    size_t i, l = strlen(name);

    if (!l || l > NAMELEN)
        return 0;
    for (i = 0; i < l; i++)
        if ((unsigned char)name[i] < 0x20 || (unsigned char)name[i] > 0x7e ||
            name[i] == '/')
            return 0;
    return 1;
}

static int find(const uint8_t *r, uint32_t n, const char *name)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        char d[NAMELEN + 1];

        disp_name(ent(r, i), d);
        if (!strcasecmp(d, name))
            return (int)i;
    }
    return -1;
}

/* Mark the chain of one file in `used`; returns 0 if well formed with exactly
 * ceil(size/B) blocks, negative otherwise (blocks seen are still marked). */
static int chain_walk(const uint8_t *r, uint32_t nb, uint32_t start,
                      uint32_t size, uint8_t *used, uint32_t *cnt)
{
    uint32_t want = (uint32_t)(((uint64_t)size + B - 1) / B), c = start, n = 0;

    for (;;) {
        uint32_t nx;

        if (c >= nb)
            return -1;
        if (used[c])
            return -2;           /* cross link or loop */
        used[c] = 1;
        n++;
        nx = map_get(r, c) & 0x7fffu;
        if (nx == FLASHFS_BLK_END)
            break;
        if (nx >= nb)
            return -3;           /* free/reserved marker mid chain */
        c = nx;
    }
    if (cnt)
        *cnt = n;
    return n == want ? 0 : -5;
}

static void used_set(const uint8_t *r, uint32_t nb, uint8_t *used)
{
    uint32_t i, n = ent_count(r);

    memset(used, 0, nb);
    for (i = 0; i < n; i++) {
        const uint8_t *e = ent(r, i);

        chain_walk(r, nb, rd16be(e + NAMELEN), rd32be(e + NAMELEN + 2), used,
                   NULL);
    }
}





static int is_erased(flashfs_wr_t *wr, uint32_t b)
{
    if (wr->nonerased[b])
        return 0;
    if (!wr->be->is_erased(wr, b)) {
        wr->nonerased[b] = 1;
        return 0;
    }
    return 1;
}

/* Blocks we must never hand out: root blocks and whatever the backend reserves. */
static int reserved_blk(const flashfs_wr_t *wr, uint32_t b)
{
    return b == wr->root_blk || b == wr->fb_blk || wr->be->reserved(wr, b);
}

/* Next block that is free in the working map, unreserved and fully erased. */
/* True if `need` erased free blocks exist (plus one for the new root block).
 * Checked BEFORE the first data write: running out halfway would leave
 * written-but-unreferenced blocks behind, and a refused operation must not
 * touch the flash at all. */
static int have_erased(flashfs_wr_t *wr, uint32_t need)
{
    uint32_t nb = wr->fs->nblocks, b, got = 0;

    need++;
    for (b = wr->hint; b < nb && got < need; b++)
        if ((map_get(wr->root, b) & 0x7fffu) == FLASHFS_BLK_FREE &&
            !reserved_blk(wr, b) && is_erased(wr, b))
            got++;
    return got >= need ? 0 : -ENOSPC;
}

static int alloc_erased(flashfs_wr_t *wr)
{
    uint32_t nb = wr->fs->nblocks, b;

    for (b = wr->hint; b < nb; b++) {
        if ((map_get(wr->root, b) & 0x7fffu) != FLASHFS_BLK_FREE ||
            reserved_blk(wr, b) || !is_erased(wr, b))
            continue;
        wr->hint = b + 1;
        return (int)b;
    }
    return -ENOSPC;
}


void flashfs_wr_close(flashfs_wr_t *wr)
{
    free(wr->root);
    free(wr->root_committed);
    free(wr->fb_root);
    free(wr->nonerased);
    free(wr->tmp);
    free(wr->raw);
    free(wr->roots);
    memset(wr, 0, sizeof(*wr));
}

int flashfs_wr_open(flashfs_wr_t *wr, flashfs_t *fs, flashfs_image_write_fn w,
                    flashfs_image_erase_fn erase, void *wctx, uint32_t now)
{
    int ret;

    memset(wr, 0, sizeof(*wr));
    if (fs->nblocks > 4096)
        return -EOPNOTSUPP;
    wr->fs = fs;
    wr->write = w;
    wr->erase = erase;
    wr->wctx = wctx;
    wr->now = now;
    wr->be = fs->emmc ? &flashfs_wr_emmc : &flashfs_wr_nand;
    wr->fb_blk = 0xffff;
    ret = wr->be->open(wr);
    if (ret)
        goto fail;
    wr->root = malloc(B);
    wr->root_committed = malloc(B);
    wr->nonerased = calloc(fs->nblocks, 1);
    wr->tmp = malloc(B);
    ret = -ENOMEM;
    if (!wr->root || !wr->root_committed || !wr->nonerased || !wr->tmp)
        goto fail;
    ret = -EIO;
    if (rd_block(wr, wr->root_blk, wr->root_committed))
        goto fail;
    /* single root block only: the root's own map slot must end the chain */
    {
        uint16_t nx = map_get(wr->root_committed, wr->root_blk) & 0x7fffu;

        ret = -EOPNOTSUPP;
        if (nx != FLASHFS_BLK_FREE && nx != FLASHFS_BLK_END)
            goto fail;
    }
    memcpy(wr->root, wr->root_committed, B);
    if (wr->fb_blk != 0xffff) {
        wr->fb_root = malloc(B);
        ret = -EIO;
        if (!wr->fb_root || rd_block(wr, wr->fb_blk, wr->fb_root))
            goto fail;
    }
    return 0;
fail:
    flashfs_wr_close(wr);
    return ret;
}

int flashfs_wr_abort(flashfs_wr_t *wr)
{
    uint32_t b, nb = wr->fs->nblocks;

    /* wipe data blocks this session wrote (free before, taken now) */
    for (b = 0; b < nb; b++)
        if ((map_get(wr->root_committed, b) & 0x7fffu) == FLASHFS_BLK_FREE &&
            (map_get(wr->root, b) & 0x7fffu) != FLASHFS_BLK_FREE) {
            wr->be->wipe(wr, b);
            wr->nonerased[b] = 0;
        }
    memcpy(wr->root, wr->root_committed, B);
    wr->hint = 0;
    wr->dirty = 0;
    return 0;
}

static void free_chain(flashfs_wr_t *wr, uint32_t start)
{
    uint32_t c = start, guard = 0, nb = wr->fs->nblocks;

    while (c < nb && guard++ < nb) {
        uint32_t nx = map_get(wr->root, c) & 0x7fffu;

        map_set(wr->root, c, FLASHFS_BLK_FREE);
        if (nx == FLASHFS_BLK_END || nx >= nb)
            break;
        c = nx;
    }
}

int flashfs_wr_patch(flashfs_wr_t *wr, uint32_t blk, uint32_t off,
                     const void *data, uint32_t len)
{
    if (blk >= wr->fs->nblocks || off + (uint64_t)len > B)
        return -EINVAL;
    return wr->be->patch(wr, blk, off, data, len);
}

/* Write bytes inside the current size of a file, in place (no metadata). */
int flashfs_wr_write(flashfs_wr_t *wr, const char *name, uint32_t off,
                     const void *data, uint32_t len)
{
    uint32_t n = ent_count(wr->root), nb = wr->fs->nblocks, size, c, i, skip;
    int idx = find(wr->root, n, name), ret;
    const uint8_t *e, *src = data;

    if (idx < 0)
        return -ENOENT;
    e = ent(wr->root, (uint32_t)idx);
    size = rd32be(e + NAMELEN + 2);
    if ((uint64_t)off + len > size)
        return -EINVAL;
    c = rd16be(e + NAMELEN);
    for (skip = off / B, i = 0; i < skip; i++) {
        c = map_get(wr->root, c) & 0x7fffu;
        if (c >= nb)
            return -EIO;
    }
    off %= B;
    while (len) {
        uint32_t chunk = B - off < len ? B - off : len;

        ret = wr->be->patch(wr, c, off, src, chunk);
        if (ret)
            return ret;
        src += chunk;
        len -= chunk;
        off = 0;
        if (len) {
            c = map_get(wr->root, c) & 0x7fffu;
            if (c >= nb)
                return -EIO;
        }
    }
    return 0;
}

int flashfs_wr_put(flashfs_wr_t *wr, const char *name, const void *data,
                   uint32_t len)
{
    uint32_t n = ent_count(wr->root), nblk, i;
    int idx, first = -1, prev = -1, ret;
    const uint8_t *src = data;
    uint8_t *e;

    if (!name_ok(name) || !len)
        return -EINVAL;
    idx = find(wr->root, n, name);
    if (idx >= 0) {
        /* existing file: no copy. Adjust the size (allocating only the extra
         * blocks, or releasing the tail) and overwrite the data in place. */
        uint32_t old = rd32be(ent(wr->root, (uint32_t)idx) + NAMELEN + 2);

        if (len != old) {
            ret = flashfs_wr_resize(wr, name, len);
            if (ret)
                return ret;
        }
        ret = flashfs_wr_write(wr, name, 0, data, len);
        if (ret) {
            flashfs_wr_abort(wr);
            return ret;
        }
        wr->dirty = 1;
        return 0;
    }
    if (n >= FLASHFS_MAX_ENTRIES)
        return -ENOSPC;
    nblk = (uint32_t)(((uint64_t)len + B - 1) / B);
    if (have_erased(wr, nblk))
        return -ENOSPC;
    for (i = 0; i < nblk; i++) {
        uint32_t chunk = len - i * B < B ? len - i * B : B;
        int blk = alloc_erased(wr);

        if (blk < 0) {
            ret = blk;
            goto fail;
        }
        memset(wr->tmp, 0, B);          /* tail padding is zero like the OS */
        memcpy(wr->tmp, src + (size_t)i * B, chunk);
        ret = wr->be->fresh(wr, (uint32_t)blk, wr->tmp);
        wr->nonerased[blk] = 1;
        map_set(wr->root, (uint32_t)blk, FLASHFS_BLK_END);
        if (ret)
            goto fail;
        if (prev >= 0)
            map_set(wr->root, (uint32_t)prev, (uint16_t)blk);
        else
            first = blk;
        prev = blk;
    }
    e = ent(wr->root, n);
    memset(e, 0, FLASHFS_ENTRY_LEN);
    memcpy(e, name, strlen(name));
    put16(e + NAMELEN, (uint16_t)first);
    put32(e + NAMELEN + 2, len);
    put32(e + NAMELEN + 6, wr->now);
    wr->dirty = 1;
    return 0;
fail:
    flashfs_wr_abort(wr);
    return ret;
}

int flashfs_wr_rm(flashfs_wr_t *wr, const char *name)
{
    uint32_t n = ent_count(wr->root), i;
    int idx = find(wr->root, n, name);

    if (idx < 0)
        return -ENOENT;
    free_chain(wr, rd16be(ent(wr->root, (uint32_t)idx) + NAMELEN));
    for (i = (uint32_t)idx; i + 1 < n; i++)
        memcpy(ent(wr->root, i), ent(wr->root, i + 1), FLASHFS_ENTRY_LEN);
    memset(ent(wr->root, n - 1), 0, FLASHFS_ENTRY_LEN);
    wr->dirty = 1;
    return 0;
}

int flashfs_wr_resize(flashfs_wr_t *wr, const char *name, uint32_t size)
{
    uint32_t n = ent_count(wr->root), old, ob, nbk, c, i, guard = 0;
    int idx = find(wr->root, n, name), ret;
    uint8_t *e;

    if (idx < 0)
        return -ENOENT;
    if (!size)
        return -EINVAL;
    e = ent(wr->root, (uint32_t)idx);
    old = rd32be(e + NAMELEN + 2);
    ob = (uint32_t)(((uint64_t)old + B - 1) / B);
    nbk = (uint32_t)(((uint64_t)size + B - 1) / B);
    /* walk to the current last block */
    c = rd16be(e + NAMELEN);
    for (i = 1; i < ob; i++) {
        c = map_get(wr->root, c) & 0x7fffu;
        if (c >= wr->fs->nblocks || guard++ > wr->fs->nblocks)
            return -EIO;
    }
    /* Growing: the bytes between the old EOF and the end of the old last block
     * must read as zero (shrinking leaves stale data there), whether or not new
     * blocks are needed. */
    if (nbk > ob && have_erased(wr, nbk - ob))
        return -ENOSPC;                  /* nothing written yet */
    if (size > old && old % B) {
        memset(wr->tmp, 0, B - old % B);
        if (wr->be->patch(wr, c, old % B, wr->tmp, B - old % B))
            return -EIO;
    }
    if (nbk < ob) {                      /* shrink: cut the chain after block nbk */
        uint32_t k = rd16be(e + NAMELEN), nx;

        for (i = 1; i < nbk; i++)
            k = map_get(wr->root, k) & 0x7fffu;
        nx = map_get(wr->root, k) & 0x7fffu;
        map_set(wr->root, k, FLASHFS_BLK_END);
        free_chain(wr, nx);
    } else if (nbk > ob) {               /* grow with zero blocks */
        uint32_t prev = c;

        for (i = ob; i < nbk; i++) {
            int blk = alloc_erased(wr);

            if (blk < 0) {
                ret = blk;
                goto fail;
            }
            memset(wr->tmp, 0, B);
            ret = wr->be->fresh(wr, (uint32_t)blk, wr->tmp);
            wr->nonerased[blk] = 1;
            map_set(wr->root, (uint32_t)blk, FLASHFS_BLK_END);
            if (ret)
                goto fail;
            map_set(wr->root, prev, (uint16_t)blk);
            prev = (uint32_t)blk;
        }
    }
    put32(e + NAMELEN + 2, size);
    put32(e + NAMELEN + 6, wr->now);
    wr->dirty = 1;
    return 0;
fail:
    flashfs_wr_abort(wr);
    return ret;
}

int flashfs_wr_commit(flashfs_wr_t *wr)
{
    uint32_t nb = wr->fs->nblocks, b;
    uint8_t *used_old, *used_cur, *used_new;
    int r, ret;

    if (!wr->dirty)
        return 0;
    r = alloc_erased(wr);               /* new root: never in place */
    if (r < 0)
        return r;
    ret = wr->be->commit_root(wr, (uint32_t)r);
    if (ret)
        return ret;

    /* committed. Reclaim blocks only the dropped root referenced. */
    used_old = calloc(nb, 1);
    used_cur = calloc(nb, 1);
    used_new = calloc(nb, 1);
    if (used_old && used_cur && used_new && wr->fb_root) {
        used_set(wr->fb_root, nb, used_old);
        used_set(wr->root_committed, nb, used_cur);
        used_set(wr->root, nb, used_new);
        for (b = 0; b < nb; b++)
            if (used_old[b] && !used_cur[b] && !used_new[b]) {
                if (!wr->be->wipe(wr, b))
                    wr->nonerased[b] = 0;
            }
    }
    free(used_old);
    free(used_cur);
    free(used_new);
    free(wr->fb_root);
    wr->fb_root = wr->root_committed;
    wr->root_committed = malloc(B);
    if (!wr->root_committed)
        return -ENOMEM;
    memcpy(wr->root_committed, wr->root, B);
    wr->fb_blk = wr->root_blk;
    wr->root_blk = (uint16_t)r;
    wr->be->committed(wr, (uint32_t)r);
    wr->dirty = 0;
    wr->hint = 0;
    return 0;
}

int flashfs_wr_check(flashfs_wr_t *wr, char *msg, size_t msglen)
{
    uint32_t nb = wr->fs->nblocks, n = ent_count(wr->root_committed), i;
    uint32_t nused = 0, stray = 0, b, cnt;
    uint8_t *used = calloc(nb, 1);
    const uint8_t *r = wr->root_committed;

    if (!used)
        return -ENOMEM;
    for (i = 0; i < n; i++) {
        const uint8_t *e = ent(r, i);
        char d[NAMELEN + 1];
        int ret;

        disp_name(e, d);
        ret = chain_walk(r, nb, rd16be(e + NAMELEN), rd32be(e + NAMELEN + 2),
                         used, &cnt);
        if (ret) {
            snprintf(msg, msglen, "%s: bad chain (%d)", d, ret);
            free(used);
            return -1;
        }
    }
    for (b = 0; b < nb; b++) {
        nused += used[b];
        if (!used[b] && (map_get(r, b) & 0x7fffu) != FLASHFS_BLK_FREE &&
            (map_get(r, b) & 0x7fffu) != FLASHFS_BLK_RSRVD)
            stray++;
    }
    if (used[wr->root_blk]) {
        snprintf(msg, msglen, "root block %u is also file data", wr->root_blk);
        free(used);
        return -1;
    }
    snprintf(msg, msglen,
             "ok: %u files, %u blocks in files, %u unaccounted map entries, "
             "%s %u root %u slot %d",
             n, nused, stray, wr->be->gen_name, wr->gen, wr->root_blk, wr->newest_slot);
    free(used);
    return 0;
}

int flashfs_name_valid(const char *name)
{
    return name_ok(name);
}

uint32_t flashfs_wr_nentries(const flashfs_wr_t *wr)
{
    return ent_count(wr->root);
}

int flashfs_wr_lookup(flashfs_wr_t *wr, const char *name, uint16_t *start,
                      uint32_t *size)
{
    int idx = find(wr->root, ent_count(wr->root), name);
    const uint8_t *e;

    if (idx < 0)
        return -ENOENT;
    e = ent(wr->root, (uint32_t)idx);
    *start = rd16be(e + NAMELEN);
    *size = rd32be(e + NAMELEN + 2);
    return 0;
}

int flashfs_wr_rename(flashfs_wr_t *wr, const char *from, const char *to)
{
    uint32_t n = ent_count(wr->root);
    int idx = find(wr->root, n, from);
    uint8_t *e;

    if (!name_ok(to))
        return -EINVAL;
    if (idx < 0)
        return -ENOENT;
    if (find(wr->root, n, to) >= 0 && strcasecmp(from, to))
        return -EEXIST;       /* caller removes the target first (same commit) */
    e = ent(wr->root, (uint32_t)idx);
    memset(e, 0, NAMELEN);
    memcpy(e, to, strlen(to));
    wr->dirty = 1;
    return 0;
}

void flashfs_wr_export_map(const flashfs_wr_t *wr, uint16_t *map)
{
    uint32_t i;

    for (i = 0; i < wr->fs->nblocks; i++)
        map[i] = map_get(wr->root, i);
}
