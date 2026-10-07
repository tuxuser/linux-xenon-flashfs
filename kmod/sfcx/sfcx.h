/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _XENON_SFCX_H
#define _XENON_SFCX_H

#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/io.h>
#include <linux/mtd/mtd.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include "emmc_hw.h"
#include "nand_hw.h"
#include "flashfs_priv.h"      /* SPARE_* layouts */

/*
 * The Xenon "SFC" PCI function (1414:580b) is one of two different controllers:
 *  - the original NAND controller (SFCX): raw NAND with hardware ECC -> MTD
 *  - on Corona/Winchester consoles an SDHCI-like eMMC controller ("MMCX") ->
 *    a block device (xenonflash0) driven with the commands that are known to
 *    work with it; the KSB initialised the card and it is never reset
 */
struct sfcx {
    struct pci_dev *pdev;
    void __iomem *regs;             /* exact 1K BAR, never a page mapping */
    bool is_emmc;

    /* NAND */
    struct mutex lock;              /* the NAND engine is single-threaded */
    struct mtd_info mtd;
    u32 saved_config;
    u32 pages_block;
    int spare_type;                 /* SPARE_* */
    nand_hw_t nand;                 /* shared with the host tests */
    bool nand_wr;                   /* MTD writes enabled (module params) */

    /* eMMC */
    emmc_hw_t emmc;
    struct gendisk *disk;
    struct blk_mq_tag_set tag_set;
    struct dentry *dbg;
    u32 cid[4], csd[4];             /* RESP3..RESP0 words, see emmc_hw.h */
    bool ident_ok;
};

static inline u32 sfcx_rd(struct sfcx *s, unsigned int r)
{
    return ioread32(s->regs + r);
}

static inline void sfcx_wr(struct sfcx *s, unsigned int r, u32 v)
{
    iowrite32(v, s->regs + r);
}

#define SFCX_BAR     0
#define SFCX_CONFIG  0x00
#define SFCX_HOSTVER 0xfc           /* SDHCI slot status / host version */

extern bool sfcx_allow_write;       /* the one write gate (NAND and eMMC) */

int sfcx_nand_probe(struct sfcx *s);
/* returns true if `s` may be freed */
bool sfcx_nand_remove(struct sfcx *s);
int sfcx_emmc_probe(struct sfcx *s);
void sfcx_emmc_remove(struct sfcx *s);


#endif
