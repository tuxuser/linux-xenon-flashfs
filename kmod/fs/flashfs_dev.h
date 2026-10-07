/* SPDX-License-Identifier: GPL-2.0 */
/* Storage behind a mount: a block device (source "/dev/...") or an MTD (by name). */
#ifndef _FLASHFS_DEV_H
#define _FLASHFS_DEV_H

#include <linux/err.h>
#include <linux/list.h>
#include <linux/types.h>

struct file;
struct mtd_info;

/* Flat (page_spare == 0): plain image. Raw: records of page_data + page_spare
 * bytes; write() takes whole records, erase() whole blocks of pages_block records. */
struct flashfs_dev {
    struct list_head node;
    const char *name;
    bool writer;
    u64 size;
    u32 page_data, page_spare, pages_block;
    int spare_type;
    int (*read)(struct flashfs_dev *dev, u64 offset, void *buf, size_t len);
    int (*write)(struct flashfs_dev *dev, u64 offset, const void *buf, size_t len);
    int (*erase)(struct flashfs_dev *dev, u64 offset, size_t len);
    void (*close)(struct flashfs_dev *dev);
    struct mtd_info *mtd;
    struct file *file;
};

/* ERR_PTR(-EBUSY) if a writer would share the device with anyone, -EROFS if it
 * cannot be written, -ENODEV if it does not exist. */
struct flashfs_dev *flashfs_dev_open(const char *source, bool write);
void flashfs_dev_close(struct flashfs_dev *dev);

#endif
