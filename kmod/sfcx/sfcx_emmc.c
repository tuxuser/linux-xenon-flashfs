// SPDX-License-Identifier: GPL-2.0
/*
 * eMMC personality of the Xenon SFC function.
 *
 * The controller is SDHCI-like, but the way it can be driven is only what is
 * known to work with it (see src/emmc_hw.c): CMD13/CMD8 status and EXT_CSD,
 * CMD17/CMD24 sector transfers through the PIO FIFO, and the stand-by dance
 * for CID/CSD. The KSB initialised and selected the card (RCA 0xFFFF, 8-bit
 * bus, high-speed) and it is never reset, so this is a plain block device:
 *
 *   /dev/xenonflash0               the whole eMMC (read-only unless
 *                                  allow_write=1)
 *   debugfs xenon_sfcx/ext_csd     raw 512 byte EXT_CSD, read from the card
 *   debugfs xenon_sfcx/cid, csd    identity words read at probe
 *   debugfs xenon_sfcx/status      controller and card state
 *
 * Everything that touches the controller holds s->lock; it is single-threaded
 * and one stuck command wedges the south bridge.
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include "sfcx.h"

/* ---- controller access for emmc_hw -------------------------------------- */

static u32 e_rd(void *c, unsigned int o) { return sfcx_rd(c, o); }
static void e_wr(void *c, unsigned int o, u32 v) { sfcx_wr(c, o, v); }
static u32 e_pio(void *c) { return __raw_readl(((struct sfcx *)c)->regs + 0x20); }
static void e_pio_w(void *c, u32 v) { __raw_writel(v, ((struct sfcx *)c)->regs + 0x20); }
static u64 e_now(void *c) { return ktime_to_ms(ktime_get()); }
static void e_relax(void *c) { udelay(10); }
static void e_log(void *c, const char *m, u32 a, u32 b)
{
    dev_err(&((struct sfcx *)c)->pdev->dev, "%s (cmd/arg %u, %08x)\n", m, a, b);
}

static const struct emmc_hw_ops sfcx_emmc_ops = {
    .rd = e_rd, .wr = e_wr, .pio = e_pio, .pio_write = e_pio_w,
    .now_ms = e_now, .relax = e_relax, .log = e_log,
};

/* ---- block device ------------------------------------------------------- */

static blk_status_t sfcx_queue_rq(struct blk_mq_hw_ctx *hctx,
                                  const struct blk_mq_queue_data *bd)
{
    struct request *rq = bd->rq;
    struct sfcx *s = hctx->queue->queuedata;
    blk_status_t status = BLK_STS_OK;
    sector_t sector = blk_rq_pos(rq);
    struct req_iterator iter;
    struct bio_vec bv;
    bool write = req_op(rq) == REQ_OP_WRITE;

    blk_mq_start_request(rq);
    switch (req_op(rq)) {
    case REQ_OP_FLUSH:               /* writes are programmed before they return */
        break;
    case REQ_OP_READ:
    case REQ_OP_WRITE:
        mutex_lock(&s->lock);
        rq_for_each_segment(bv, rq, iter) {
            u8 *buf = bvec_kmap_local(&bv);
            unsigned int off;

            for (off = 0; off < bv.bv_len; off += 512, sector++) {
                int ret = write ?
                    emmc_hw_write_sector(&s->emmc, sector, buf + off) :
                    emmc_hw_read(&s->emmc, (u64)sector * 512, buf + off, 512);

                if (ret) {
                    status = BLK_STS_IOERR;
                    break;
                }
            }
            kunmap_local(buf);
            if (status != BLK_STS_OK)
                break;
        }
        mutex_unlock(&s->lock);
        break;
    default:
        status = BLK_STS_NOTSUPP;
    }
    blk_mq_end_request(rq, status);
    return BLK_STS_OK;
}

static const struct blk_mq_ops sfcx_mq_ops = {
    .queue_rq = sfcx_queue_rq,
};

static const struct block_device_operations sfcx_bops = {
    .owner = THIS_MODULE,
};

/* ---- debugfs ------------------------------------------------------------- */

static ssize_t ext_csd_read(struct file *f, char __user *ubuf, size_t len, loff_t *pos)
{
    struct sfcx *s = file_inode(f)->i_private;
    u8 *buf = kmalloc(512, GFP_KERNEL);
    int ret;

    if (!buf)
        return -ENOMEM;
    mutex_lock(&s->lock);
    ret = emmc_hw_read_ext_csd(&s->emmc, buf);
    mutex_unlock(&s->lock);
    if (!ret)
        ret = simple_read_from_buffer(ubuf, len, pos, buf, 512);
    else
        ret = -EIO;
    kfree(buf);
    return ret;
}

static const struct file_operations ext_csd_fops = {
    .owner = THIS_MODULE,
    .open = simple_open,
    .read = ext_csd_read,
    .llseek = default_llseek,
};

static int ident_show(struct seq_file *m, const u32 *w, bool ok)
{
    if (!ok)
        seq_puts(m, "unavailable\n");
    else
        seq_printf(m, "%08x%08x%08x%08x\n", w[3], w[2], w[1], w[0]);
    return 0;
}

static int cid_show(struct seq_file *m, void *p)
{
    struct sfcx *s = m->private;

    return ident_show(m, s->cid, s->ident_ok);
}
DEFINE_SHOW_ATTRIBUTE(cid);

static int csd_show(struct seq_file *m, void *p)
{
    struct sfcx *s = m->private;

    return ident_show(m, s->csd, s->ident_ok);
}
DEFINE_SHOW_ATTRIBUTE(csd);

static int status_show(struct seq_file *m, void *p)
{
    struct sfcx *s = m->private;
    u32 st = 0;
    int ret;

    mutex_lock(&s->lock);
    ret = emmc_hw_card_status(&s->emmc, &st);
    seq_printf(m, "controller: SDHCI-like MMCX, host version %04x, caps %08x\n",
               sfcx_rd(s, 0xfc) >> 16, sfcx_rd(s, 0x40));
    seq_printf(m, "present state %08x, clock/timeout/reset %08x\n",
               sfcx_rd(s, 0x24), sfcx_rd(s, 0x2c));
    seq_printf(m, "card: %u sectors (%llu MiB), RCA %04x\n", s->emmc.sectors,
               (u64)s->emmc.sectors / 2048, EMMC_CARD_RCA);
    if (ret)
        seq_puts(m, "card status: unavailable\n");
    else
        seq_printf(m, "card status %08x, state %u (4 = transfer)\n", st,
                   (st >> 9) & 0xf);
    mutex_unlock(&s->lock);
    return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);

/* ---- probe / remove ------------------------------------------------------ */

int sfcx_emmc_probe(struct sfcx *s)
{
    struct queue_limits lim = {
        .logical_block_size = 512,
        .physical_block_size = 512,
        .max_hw_sectors = 256,
    };
    struct gendisk *disk;
    int ret;

    mutex_init(&s->lock);
    s->emmc.ops = &sfcx_emmc_ops;
    s->emmc.ctx = s;
    ret = emmc_hw_init(&s->emmc);
    if (ret) {
        dev_err(&s->pdev->dev, "eMMC init failed (%d)\n", ret);
        return -EIO;
    }
    /* CID/CSD: needs the card in stand-by for a moment, so only here, once */
    s->ident_ok = !emmc_hw_read_ident(&s->emmc, s->cid, s->csd);
    if (!s->ident_ok)
        dev_warn(&s->pdev->dev, "could not read CID/CSD\n");

    ret = blk_mq_alloc_sq_tag_set(&s->tag_set, &sfcx_mq_ops, 16,
                                  BLK_MQ_F_BLOCKING);
    if (ret)
        return ret;
    disk = blk_mq_alloc_disk(&s->tag_set, &lim, s);
    if (IS_ERR(disk)) {
        ret = PTR_ERR(disk);
        goto err_tags;
    }
    s->disk = disk;
    disk->fops = &sfcx_bops;
    disk->private_data = s;
    disk->flags |= GENHD_FL_NO_PART;          /* a raw flash image, no table */
    snprintf(disk->disk_name, sizeof(disk->disk_name), "xenonflash0");
    set_capacity(disk, s->emmc.sectors);
    set_disk_ro(disk, !sfcx_allow_write);
    ret = device_add_disk(&s->pdev->dev, disk, NULL);
    if (ret)
        goto err_disk;

    s->dbg = debugfs_create_dir("xenon_sfcx", NULL);
    debugfs_create_file("ext_csd", 0400, s->dbg, s, &ext_csd_fops);
    debugfs_create_file("cid", 0400, s->dbg, s, &cid_fops);
    debugfs_create_file("csd", 0400, s->dbg, s, &csd_fops);
    debugfs_create_file("status", 0400, s->dbg, s, &status_fops);
    dev_info(&s->pdev->dev, "eMMC %u MiB as /dev/%s (%s)\n",
             s->emmc.sectors / 2048, disk->disk_name,
             sfcx_allow_write ? "writable" : "read-only");
    return 0;

err_disk:
    put_disk(disk);
err_tags:
    blk_mq_free_tag_set(&s->tag_set);
    return ret;
}

void sfcx_emmc_remove(struct sfcx *s)
{
    debugfs_remove_recursive(s->dbg);
    del_gendisk(s->disk);                       /* waits for I/O in flight */
    put_disk(s->disk);
    blk_mq_free_tag_set(&s->tag_set);
}
