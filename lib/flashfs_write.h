/* flashfs_write.h - metadata writer for the eMMC flash filesystem.
 *
 * File data is never copied: existing bytes are overwritten in place, a growing
 * file only gets new blocks, a shrinking file releases its tail. The file TABLE
 * (blockmap + entries) is committed copy-on-write:
 *   1. new data blocks are allocated from ERASED (all 0xFF) blocks only. "Free in
 *      the block map" does not mean unused: the OS keeps roots and other data in
 *      such blocks, so reusing dirty free blocks is unsafe.
 *   2. the new root block (modified blockmap + entries, all unknown bytes kept)
 *      goes to another erased block,
 *   3. the commit is a single 512-byte write of the OLDER root-pointer slot
 *      with version+1, the new root block, otherwise identical header fields and a fresh
 *      SHA-1 over slot[0x14:0x200].
 * The previous root and the other slot are never touched, so an invalid or
 * unwanted new slot always leaves the previous state recoverable.
 * Data blocks that fall out of the (newest, fallback) root pair are reclaimed by
 * filling them with 0xFF after the commit; superseded root blocks are left alone.
 *
 * NAND (small block, spare-mapped) uses the same table logic over a block store
 * with three primitives: program a FRESH erased block, erase a block, patch bytes
 * of a block IN PLACE (erase + reprogram of that same physical block, spare bytes
 * kept; no copy elsewhere). The commit there is a new root block in a fresh erased
 * block with the spare sequence number + 1 (the reader mounts the newest sane
 * root); the root's pages are programmed so that a torn write leaves a root the
 * reader rejects: pages 2..31, then 0 (spare/seq visible), the first entries page
 * (page 1) LAST. The root before the new one is kept as fallback and older roots
 * are erased. All spare bytes the writer does not understand are copied.
 *
 * Only single-root-block filesystems are supported.
 */
#ifndef _FLASHFS_WRITE_H
#define _FLASHFS_WRITE_H

#include "flashfs_priv.h"

/* eMMC: byte offsets in the flat image. NAND: offsets in the RAW RECORD space
 * (528 bytes per page = 512 data + 16 spare), always whole page records; the
 * callee generates the ECC in spare[12..15] and keeps spare[12] bits 0..5. */
typedef int (*flashfs_image_write_fn)(void *ctx, uint64_t offset,
                                      const void *buffer, size_t length);
/* NAND only: erase one physical block. */
typedef int (*flashfs_image_erase_fn)(void *ctx, uint32_t phys_block);

#define FLASHFS_MAX_ENTRIES   256u   /* 16 pages x 16 entries */
#define FLASHFS_SLOT_HASH_OFF 0x14u
#define FLASHFS_SLOT_LEN      0x200u /* hashed + written as one sector */
#define FLASHFS_SLOT_VER_OFF  0x18u
#define FLASHFS_SLOT_ROOT_OFF 0x1cu

struct flashfs_wr_backend;

typedef struct {
    flashfs_t *fs;
    flashfs_image_write_fn write;
    flashfs_image_erase_fn erase;   /* NAND */
    void *wctx;
    const struct flashfs_wr_backend *be;
    uint32_t gen;             /* eMMC: slot version, NAND: root spare sequence */
    uint8_t *raw;             /* NAND: one physical block of raw records */
    fs_candidate_t *roots;    /* NAND: every known root block (lil, seq) */
    uint32_t nroots;
    uint8_t *root;            /* working root block */
    uint8_t *root_committed;  /* root block as currently on flash */
    uint8_t *fb_root;         /* root of the fallback slot (NULL if none) */
    uint8_t *nonerased;       /* per block: known not erased */
    uint8_t *tmp;             /* one block of scratch */
    uint8_t slot[FLASHFS_SLOT_LEN]; /* eMMC: newest slot header */
    uint8_t next_slot[FLASHFS_SLOT_LEN]; /* eMMC: header being committed */
    int next_target;
    uint16_t root_blk, fb_blk;
    int newest_slot;          /* 0 or 1 */
    uint32_t hint;            /* allocator scan position */
    uint32_t now;             /* timestamp for new/changed entries */
    int dirty;
} flashfs_wr_t;

int flashfs_wr_open(flashfs_wr_t *wr, flashfs_t *fs, flashfs_image_write_fn w,
                    flashfs_image_erase_fn erase, void *wctx, uint32_t now);
void flashfs_wr_close(flashfs_wr_t *wr);

/* Create a file, or set an existing file's content: never copies file data.
 * An existing file is resized (only the extra blocks are allocated / the tail
 * released) and its bytes overwritten in place. len must be > 0. */
int flashfs_wr_put(flashfs_wr_t *wr, const char *name, const void *data,
                   uint32_t len);
/* Overwrite bytes of one block (logical block number, as in the block map) in
 * place: eMMC writes the bytes; NAND erases and reprograms that same physical
 * block with its spare kept. Does not touch the table. */
int flashfs_wr_patch(flashfs_wr_t *wr, uint32_t blk, uint32_t off,
                     const void *data, uint32_t len);
/* Overwrite bytes inside the current size of a file, in place. */
int flashfs_wr_write(flashfs_wr_t *wr, const char *name, uint32_t off,
                     const void *data, uint32_t len);
int flashfs_wr_rm(flashfs_wr_t *wr, const char *name);
/* Grow (zero filled) or shrink; new size must be > 0. */
int flashfs_wr_resize(flashfs_wr_t *wr, const char *name, uint32_t size);
/* Look up an entry in the working root. Returns 0 and fills start/size. */
int flashfs_wr_lookup(flashfs_wr_t *wr, const char *name, uint16_t *start,
                      uint32_t *size);
/* Rename; the target must not exist. */
int flashfs_wr_rename(flashfs_wr_t *wr, const char *from, const char *to);
/* Copy the working block map (fs->nblocks words, hibit kept) to `map`. */
void flashfs_wr_export_map(const flashfs_wr_t *wr, uint16_t *map);
uint32_t flashfs_wr_nentries(const flashfs_wr_t *wr);
int flashfs_name_valid(const char *name);
/* Write root block + slot. No-op when nothing changed. */
int flashfs_wr_commit(flashfs_wr_t *wr);
/* Drop uncommitted changes (reload the working root). */
int flashfs_wr_abort(flashfs_wr_t *wr);

/* Consistency check of the newest root. Returns 0 if clean, else -1 and fills
 * msg. Always fills `*nfiles`, `*nused` when non-NULL. */
int flashfs_wr_check(flashfs_wr_t *wr, char *msg, size_t msglen);

#endif
