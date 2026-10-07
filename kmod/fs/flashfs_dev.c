// SPDX-License-Identifier: GPL-2.0
#include <linux/blkdev.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/slab.h>
#include "flashfs_priv.h"
#include "flashfs_dev.h"

static LIST_HEAD(open_devs);
static DEFINE_MUTEX(open_lock);
static int bdev_holder;                  /* any unique address */

/* ---- block device: flat image, every write is fsync'ed ------------------ */

static int bdev_read(struct flashfs_dev *d, u64 offset, void *buf, size_t len)
{
    loff_t pos = offset;

    while (len) {
        ssize_t n = kernel_read(d->file, buf, len, &pos);

        if (n <= 0)
            return n ? n : -EIO;
        buf += n, len -= n;
    }
    return 0;
}

static int bdev_write(struct flashfs_dev *d, u64 offset, const void *buf, size_t len)
{
    loff_t pos = offset;
    u64 end = offset + len - 1;

    while (len) {
        ssize_t n = kernel_write(d->file, buf, len, &pos);

        if (n <= 0)
            return n ? n : -EIO;
        buf += n, len -= n;
    }
    return vfs_fsync_range(d->file, offset, end, 1);
}

static void bdev_close(struct flashfs_dev *d)
{
    fput(d->file);
}

static int bdev_open(struct flashfs_dev *d, const char *path, bool write)
{
    struct file *file = bdev_file_open_by_path(
        path, BLK_OPEN_READ | (write ? BLK_OPEN_WRITE : 0), &bdev_holder, NULL);

    if (IS_ERR(file))
        return PTR_ERR(file);
    d->file = file;
    d->size = bdev_nr_bytes(file_bdev(file));
    d->read = bdev_read;
    d->write = write ? bdev_write : NULL;
    d->close = bdev_close;
    return 0;
}

/* ---- MTD: flat (no OOB) or raw NAND in the page-record dump view -------- */

static u32 mtd_stride(const struct mtd_info *mtd)
{
    return mtd->writesize + mtd->oobsize;
}

static int mtd_raw_read(struct flashfs_dev *d, u64 offset, void *buf, size_t len)
{
    struct mtd_info *mtd = d->mtd;
    u32 stride = mtd_stride(mtd);
    u8 rec[512 + 64], *out = buf;

    if (stride > sizeof(rec))
        return -EINVAL;
    while (len) {
        u64 page = div_u64(offset, stride);
        u32 in = offset - page * stride;
        size_t n = min_t(size_t, len, stride - in);
        struct mtd_oob_ops ops = {
            .mode = MTD_OPS_RAW,
            .len = mtd->writesize, .datbuf = rec,
            .ooblen = mtd->oobsize, .oobbuf = rec + mtd->writesize,
        };
        int ret = mtd_read_oob(mtd, page * mtd->writesize, &ops);

        if (ret && !mtd_is_bitflip(ret))   /* -EUCLEAN: corrected, data valid */
            return ret;
        memcpy(out, rec + in, n);
        out += n, offset += n, len -= n;
    }
    return 0;
}

static int mtd_raw_write(struct flashfs_dev *d, u64 offset, const void *buf, size_t len)
{
    struct mtd_info *mtd = d->mtd;
    u32 stride = mtd_stride(mtd);
    const u8 *in = buf;
    u8 rec[512 + 64];

    if (stride > sizeof(rec) || (offset % stride) || (len % stride))
        return -EINVAL;
    for (; len; offset += stride, in += stride, len -= stride) {
        struct mtd_oob_ops ops = {
            .mode = MTD_OPS_PLACE_OOB,
            .len = mtd->writesize, .datbuf = rec,
            .ooblen = mtd->oobsize, .oobbuf = rec + mtd->writesize,
        };
        int ret;

        memcpy(rec, in, stride);
        ret = mtd_write_oob(mtd, div_u64(offset, stride) * mtd->writesize, &ops);
        if (ret)
            return ret;
        if (ops.retlen != mtd->writesize || ops.oobretlen != mtd->oobsize)
            return -EIO;
    }
    return 0;
}

static int mtd_raw_erase(struct flashfs_dev *d, u64 offset, size_t len)
{
    struct mtd_info *mtd = d->mtd;
    u32 bstride = d->pages_block * mtd_stride(mtd);
    struct erase_info ei = { };

    if ((offset % bstride) || (len % bstride) || !len)
        return -EINVAL;
    ei.addr = div_u64(offset, bstride) * mtd->erasesize;
    ei.len = div_u64(len, bstride) * mtd->erasesize;
    return mtd_erase(mtd, &ei);
}

static int mtd_flat_read(struct flashfs_dev *d, u64 offset, void *buf, size_t len)
{
    size_t got;
    int ret = mtd_read(d->mtd, offset, len, &got, buf);

    return ((ret && !mtd_is_bitflip(ret)) || got != len) ? -EIO : 0;
}

static int mtd_flat_write(struct flashfs_dev *d, u64 offset, const void *buf, size_t len)
{
    size_t done;
    int ret = mtd_write(d->mtd, offset, len, &done, buf);

    return ret ? ret : (done == len ? 0 : -EIO);
}

static void mtd_close(struct flashfs_dev *d)
{
    put_mtd_device(d->mtd);
}

static int mtd_open(struct flashfs_dev *d, const char *name, bool write)
{
    struct mtd_info *mtd = get_mtd_device_nm(name);
    bool raw;

    if (IS_ERR(mtd))
        return -ENODEV;
    raw = mtd->oobsize;
    d->mtd = mtd;
    d->close = mtd_close;
    if (raw) {
        /* the controller driver attaches the layout to its device */
        if (!mtd->dev.parent ||
            device_property_read_u32(mtd->dev.parent, "xenon,spare-type",
                                     &d->spare_type)) {
            put_mtd_device(mtd);
            return -ENODEV;
        }
        d->page_data = mtd->writesize;
        d->page_spare = mtd->oobsize;
        d->pages_block = mtd->erasesize / mtd->writesize;
        d->size = div_u64(mtd->size, mtd->writesize) * mtd_stride(mtd);
        d->read = mtd_raw_read;
    } else {
        d->size = mtd->size;
        d->read = mtd_flat_read;
    }
    if (write) {
        bool ok = (mtd->flags & MTD_WRITEABLE) &&
                  (raw ? mtd->_write_oob && mtd->_erase && d->spare_type == SPARE_SB
                       : !!mtd->_write);

        if (!ok) {
            put_mtd_device(mtd);
            return -EROFS;
        }
        d->write = raw ? mtd_raw_write : mtd_flat_write;
        d->erase = raw ? mtd_raw_erase : NULL;
    }
    return 0;
}

/* ---- open/close with one-writer-or-many-readers per source -------------- */

struct flashfs_dev *flashfs_dev_open(const char *source, bool write)
{
    struct flashfs_dev *d, *o;
    int ret;

    d = kzalloc(sizeof(*d), GFP_KERNEL);
    if (!d)
        return ERR_PTR(-ENOMEM);
    d->name = kstrdup(source, GFP_KERNEL);
    if (!d->name) {
        kfree(d);
        return ERR_PTR(-ENOMEM);
    }
    d->writer = write;
    mutex_lock(&open_lock);
    list_for_each_entry(o, &open_devs, node)
        if (!strcmp(o->name, source) && (write || o->writer)) {
            ret = -EBUSY;
            goto err;
        }
    ret = source[0] == '/' ? bdev_open(d, source, write) : mtd_open(d, source, write);
    if (ret)
        goto err;
    list_add(&d->node, &open_devs);
    mutex_unlock(&open_lock);
    return d;
err:
    mutex_unlock(&open_lock);
    kfree(d->name);
    kfree(d);
    return ERR_PTR(ret);
}

void flashfs_dev_close(struct flashfs_dev *d)
{
    mutex_lock(&open_lock);
    list_del(&d->node);
    mutex_unlock(&open_lock);
    d->close(d);
    kfree(d->name);
    kfree(d);
}
