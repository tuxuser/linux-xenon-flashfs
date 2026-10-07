// SPDX-License-Identifier: GPL-2.0
/*
 * NAND personality of the Xenon SFC: raw NAND (512 byte pages + 16 byte spare,
 * hardware ECC) exposed as an MTD, read-only unless writing is enabled with
 * allow_write=1 (then the whole flash is writable).
 * The config word tells the controller
 * generation, which gives geometry and the spare layout; filesystem drivers get
 * the layout from the "xenon,spare-type" device property instead of a user hint.
 */
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/unaligned.h>
#include "sfcx.h"

#define SFCX_STATUS         0x04   /* only used for the interrupt/config setup */

#define NAND_CFG_TYPE(c)   (((c) >> 17) & 3)
#define NAND_CFG_SIZE(c)   (((c) >> 4) & 3)

struct nand_geo {
    u32 block_sz, size_bytes;
    int meta_type;                   /* 0 pre-Jasper, 1 Jasper small, 2 large */
};

/*
 * Decode the SFC config word the same way libxenon's sfcx_init() does. This is
 * the controller's own identification of the attached NAND, and it also tells
 * the spare/OOB layout (meta type), which MTD itself has no way to express.
 */
static int nand_decode(u32 config, struct nand_geo *g)
{
    switch (NAND_CFG_TYPE(config)) {
    case 0:                          /* original small block SFC */
        g->meta_type = 0;
        g->block_sz = 0x4000;
        switch (NAND_CFG_SIZE(config)) {
        case 1: g->size_bytes = 16u << 20; return 0;
        case 2: g->size_bytes = 32u << 20; return 0;
        case 3: g->size_bytes = 64u << 20; return 0;
        default: return -ENODEV;     /* 8 MB: unsupported */
        }
    case 1:                          /* "Panda" */
    case 2:                          /* "Panda" v2 */
        switch (NAND_CFG_SIZE(config)) {
        case 0:
            if (NAND_CFG_TYPE(config) == 1)
                return -ENODEV;
            g->meta_type = 1;
            g->block_sz = 0x4000;
            g->size_bytes = 16u << 20;
            return 0;
        case 1:
            g->meta_type = 1;
            g->block_sz = 0x4000;
            g->size_bytes = NAND_CFG_TYPE(config) == 1 ? 16u << 20 : 64u << 20;
            return 0;
        case 2:
        case 3:                      /* large block 128K / 256K */
            g->meta_type = 2;
            g->block_sz = NAND_CFG_SIZE(config) == 2 ? 0x20000 : 0x40000;
            g->size_bytes = 1u << (((config >> 19) & 3) +
                                   ((config >> 21) & 0xf) + 0x17);
            return 0;
        }
    }
    return -ENODEV;
}

static int nand_spare_type(const struct nand_geo *g)
{
    if (g->meta_type == 0)
        return SPARE_SB;
    if (g->meta_type == 1 && g->block_sz / 512 == 32)
        return SPARE_JASPER;
    return SPARE_BB;
}

static u32 nand_op_rd(void *ctx, unsigned int off) { return sfcx_rd(ctx, off); }
static void nand_op_wr(void *ctx, unsigned int off, u32 v) { sfcx_wr(ctx, off, v); }
static u64 nand_op_now(void *ctx) { return jiffies_to_msecs(get_jiffies_64()); }
static void nand_op_relax(void *ctx) { udelay(2); }

static const struct nand_hw_ops nand_ops = {
    .rd = nand_op_rd, .wr = nand_op_wr, .now_ms = nand_op_now,
    .relax = nand_op_relax,
};

static int nand_setup(struct sfcx *s, struct mtd_info *m)
{
    struct nand_geo g;
    int ret = nand_decode(sfcx_rd(s, SFCX_CONFIG), &g);

    if (ret)
        return ret;
    s->saved_config = sfcx_rd(s, SFCX_CONFIG);
    /* interrupts, write protect release and DMA off: polled raw reads only */
    sfcx_wr(s, SFCX_CONFIG, s->saved_config & ~(0x4 | 0x8 | 0x3c0));
    s->pages_block = g.block_sz / 512;
    s->spare_type = nand_spare_type(&g);
    m->name = "xenon-nand-raw";
    m->type = MTD_NANDFLASH;
    m->flags = MTD_CAP_NANDFLASH | MTD_NO_ERASE;
    m->writesize = 512;
    m->oobsize = 16;
    m->erasesize = g.block_sz;
    m->size = g.size_bytes;
    s->nand.ops = &nand_ops;
    s->nand.ctx = s;
    s->nand.pages_per_block = s->pages_block;
    s->nand.blocks = g.size_bytes / g.block_sz;
    s->nand.writable = g.meta_type == 0;     /* only layout with a verified ECC */
    s->nand_wr = sfcx_allow_write && s->nand.writable;
    if (s->nand_wr)
        m->flags = MTD_CAP_NANDFLASH | MTD_WRITEABLE;
    else if (sfcx_allow_write)
        dev_warn(&s->pdev->dev, "allow_write ignored: programming is only "
                 "supported for the small-block layout\n");
    dev_info(&s->pdev->dev, "NAND %u MiB, %u KiB blocks, spare layout %s\n",
             g.size_bytes >> 20, g.block_sz >> 10,
             s->spare_type == SPARE_SB ? "small-block" :
             s->spare_type == SPARE_JASPER ? "jasper" : "big-block");
    if (s->nand_wr)
        dev_warn(&s->pdev->dev, "NAND is WRITABLE (whole flash)\n");
    return 0;
}

static void nand_teardown(struct sfcx *s)
{
    sfcx_wr(s, SFCX_CONFIG, s->saved_config);
}

/* data = 512 bytes, oob = 16 bytes (either may be NULL). */
static int nand_read_page(struct sfcx *s, u32 page, u8 *data, u8 *oob)
{
    u8 raw[NAND_RAW_PAGE];
    int ret = nand_hw_read_page(&s->nand, page, raw);

    if (ret)
        return ret;
    if (data)
        memcpy(data, raw, 512);
    if (oob)
        memcpy(oob, raw + 512, 16);
    /* ECC bits in the status mean "corrected": the raw view returns the page */
    return 0;
}

/* Bad block marker: first page's spare, byte 5 (small block, Jasper) or byte 0
 * (big block) - see spare_decode() in flashfs_spare.c. */
static int nand_isbad(struct sfcx *s, u32 block)
{
    u8 oob[16];
    int ret = nand_read_page(s, block * s->pages_block, NULL, oob);

    if (ret)
        return ret;
    return (s->spare_type == SPARE_BB ? oob[0] : oob[5]) != 0xff;
}

/* NAND: the only read entry point (core forbids _read + _read_oob together;
 * mtd_read() is translated into this with no oobbuf). Data may start
 * mid-page; OOB is returned raw and only page-aligned. */
static int sfcx_mtd_read_oob(struct mtd_info *mtd, loff_t from,
                             struct mtd_oob_ops *ops)
{
    struct sfcx *s = container_of(mtd, struct sfcx, mtd);
    u8 *data = ops->datbuf, *oob = ops->oobbuf;
    size_t len = data ? ops->len : 0, ooblen = oob ? ops->ooblen : 0;
    u32 in = from & (mtd->writesize - 1);
    u32 page = div_u64(from, mtd->writesize);
    int ret = 0;

    if (oob && (ops->ooboffs || in))
        return -EINVAL;
    if (oob && ops->mode == MTD_OPS_AUTO_OOB)
        return -EOPNOTSUPP;     /* no ooblayout yet; raw/place only */
    ops->retlen = ops->oobretlen = 0;
    mutex_lock(&s->lock);
    while (len || ooblen) {
        u8 pg[512], po[16];

        ret = nand_read_page(s, page, pg, oob ? po : NULL);
        if (ret && ret != -EUCLEAN)
            break;
        if (len) {
            size_t n = min_t(size_t, len, mtd->writesize - in);

            memcpy(data, pg + in, n);
            data += n, len -= n, ops->retlen += n;
            in = 0;
        }
        if (ooblen) {
            size_t n = min_t(size_t, ooblen, mtd->oobsize);

            memcpy(oob, po, n);
            oob += n, ooblen -= n, ops->oobretlen += n;
        }
        page++;
    }
    mutex_unlock(&s->lock);
    return ret;
}

/*
 * Program whole pages with their spare. A data-only write is refused: a blank
 * spare carries no block metadata, which silently corrupts a small-block NAND.
 * The ECC in spare[12..15] is generated here. Bad blocks are refused and a
 * bad-block marker is never written.
 */
static int sfcx_mtd_write_oob(struct mtd_info *mtd, loff_t to,
                              struct mtd_oob_ops *ops)
{
    struct sfcx *s = container_of(mtd, struct sfcx, mtd);
    const u8 *data = ops->datbuf, *oob = ops->oobbuf;
    u32 page = div_u64(to, 512), npages, i;
    u32 checked = ~0u;
    int ret = 0;

    ops->retlen = ops->oobretlen = 0;
    if (!s->nand_wr)
        return -EROFS;
    if (ops->mode == MTD_OPS_AUTO_OOB)
        return -EOPNOTSUPP;
    if (!data || !oob || (to & 511) || ops->ooboffs || !ops->len ||
        (ops->len & 511) || ops->ooblen != (ops->len / 512) * 16)
        return -EINVAL;
    npages = ops->len / 512;
    if (to + ops->len > mtd->size)
        return -EINVAL;
    mutex_lock(&s->lock);
    for (i = 0; i < npages; i++, page++) {
        u32 block = page / s->pages_block;
        u8 raw[NAND_RAW_PAGE];

        if (block != checked) {
            ret = nand_hw_block_marked_bad(&s->nand, block);
            if (ret) {
                ret = ret < 0 ? ret : -EIO;
                break;
            }
            checked = block;
        }
        if (!(page % s->pages_block) && oob[i * 16 + 5] != 0xff) {
            ret = -EPERM;          /* would mark the block bad */
            break;
        }
        memcpy(raw, data + i * 512, 512);
        memcpy(raw + 512, oob + i * 16, 16);
        ret = nand_hw_write_page(&s->nand, page, raw);
        if (ret) {
            dev_err(&s->pdev->dev, "write page %u failed: %d (status %08x)\n",
                    page, ret, s->nand.last_status);
            break;
        }
        ops->retlen += 512;
        ops->oobretlen += 16;
    }
    mutex_unlock(&s->lock);
    return ret;
}

static int sfcx_mtd_erase(struct mtd_info *mtd, struct erase_info *instr)
{
    struct sfcx *s = container_of(mtd, struct sfcx, mtd);
    u32 block = div_u64(instr->addr, mtd->erasesize);
    u32 n = div_u64(instr->len, mtd->erasesize), i;
    int ret = 0;

    if (!s->nand_wr)
        return -EROFS;
    if ((instr->addr % mtd->erasesize) || (instr->len % mtd->erasesize))
        return -EINVAL;
    mutex_lock(&s->lock);
    for (i = 0; i < n && !ret; i++, block++) {
        ret = nand_hw_block_marked_bad(&s->nand, block);
        if (ret > 0)
            ret = -EIO;
        if (!ret)
            ret = nand_hw_erase_block(&s->nand, block);
        if (ret)
            instr->fail_addr = (u64)block * mtd->erasesize;
    }
    mutex_unlock(&s->lock);
    return ret;
}

static int sfcx_mtd_block_isbad(struct mtd_info *mtd, loff_t ofs)
{
    struct sfcx *s = container_of(mtd, struct sfcx, mtd);
    int ret;

    mutex_lock(&s->lock);
    ret = nand_isbad(s, div_u64(ofs, mtd->erasesize));
    mutex_unlock(&s->lock);
    return ret;
}

/* No device-tree partition parsing ("ofpart"): this MTD has no DT node of its
 * own and the generic code would call of_platform_populate(NULL, ...) with the
 * MTD as parent, adopting unrelated top-level DT nodes whose references are
 * never dropped (see docs/mtd-design.md, "Teardown"). mtdparts= still works. */
static const char * const sfcx_part_probes[] = { "cmdlinepart", NULL };

int sfcx_nand_probe(struct sfcx *s)
{
    int ret;

    mutex_init(&s->lock);
    ret = nand_setup(s, &s->mtd);
    if (ret)
        return ret;
    {
        struct property_entry props[] = {
            PROPERTY_ENTRY_U32("xenon,spare-type", s->spare_type),
            { }
        };

        /* before the MTD exists: adapters read it while the MTD is announced */
        ret = device_create_managed_software_node(&s->pdev->dev, props, NULL);
        if (ret) {
            nand_teardown(s);
            return ret;
        }
    }
    s->mtd.owner = THIS_MODULE;
    s->mtd.dev.parent = &s->pdev->dev;
    s->mtd._read_oob = sfcx_mtd_read_oob;
    s->mtd._block_isbad = sfcx_mtd_block_isbad;
    if (s->nand_wr) {
        s->mtd._write_oob = sfcx_mtd_write_oob;
        s->mtd._erase = sfcx_mtd_erase;
    }
    ret = mtd_device_parse_register(&s->mtd, sfcx_part_probes, NULL, NULL, 0);
    if (ret)
        nand_teardown(s);
    return ret;
}

/*
 * Holders of the device (open /dev/mtdN, a mounted filesystem) take a module
 * reference, so we only get here once nobody uses it. After unregister the core
 * releases the embedded mtd->dev and finally zeroes it; wait for that before
 * the memory goes away. If it never finishes, leak instead of freeing.
 */
bool sfcx_nand_remove(struct sfcx *s)
{
    int ret = mtd_device_unregister(&s->mtd);
    unsigned int i;

    for (i = 0; !ret && s->mtd.dev.kobj.state_initialized && i < 100; i++)
        msleep(50);
    if (ret || s->mtd.dev.kobj.state_initialized) {
        dev_err(&s->pdev->dev, "mtd still referenced after unregister (%d); "
                "leaking the device structure\n", ret);
        return false;
    }
    nand_teardown(s);
    return true;
}

