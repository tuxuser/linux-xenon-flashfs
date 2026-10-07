// SPDX-License-Identifier: GPL-2.0
/*
 * xenon_sfcx - PCI driver for the Xenon flash controller function (1414:580b).
 *
 * Picks the right personality at probe time and keeps this file small:
 *   sfcx_nand.c   raw NAND controller -> MTD
 *   sfcx_emmc.c   eMMC controller     -> block device + debugfs
 *
 * The BAR is mapped exactly (1K) with pcim_iomap_region(). With 64K pages a
 * page-granular mapping starts at 0xea000000, the south bridge, whose +0x0c is
 * the interrupt mask register.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include "sfcx.h"

bool sfcx_allow_write;
module_param_named(allow_write, sfcx_allow_write, bool, 0444);
MODULE_PARM_DESC(allow_write, "make the flash writable, NAND (whole flash) or eMMC "
                 "(default: read-only)");

static bool sfcx_is_emmc(struct sfcx *s)
{
    u32 v = sfcx_rd(s, SFCX_HOSTVER);

    /* same test as libxenon's sfcx_init(): non-zero (and not all ones) */
    return v && v != 0xffffffffu;
}

static int sfcx_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct sfcx *s;
    int ret;

    s = kzalloc(sizeof(*s), GFP_KERNEL);
    if (!s)
        return -ENOMEM;
    ret = pcim_enable_device(pdev);
    if (ret)
        goto err_free;
    s->regs = pcim_iomap_region(pdev, SFCX_BAR, "xenon-sfcx");
    if (IS_ERR(s->regs)) {
        ret = PTR_ERR(s->regs);
        goto err_free;
    }
    s->pdev = pdev;
    pci_set_drvdata(pdev, s);
    s->is_emmc = sfcx_is_emmc(s);
    ret = s->is_emmc ? sfcx_emmc_probe(s) : sfcx_nand_probe(s);
    if (ret)
        goto err_free;
    return 0;

err_free:
    kfree(s);
    return ret;
}

static void sfcx_remove(struct pci_dev *pdev)
{
    struct sfcx *s = pci_get_drvdata(pdev);

    if (s->is_emmc) {
        sfcx_emmc_remove(s);
        kfree(s);
    } else if (sfcx_nand_remove(s)) {
        kfree(s);
    }                                /* else: leaked on purpose, see sfcx_nand.c */
}

static const struct pci_device_id sfcx_ids[] = {
    { PCI_VDEVICE(MICROSOFT, 0x580b) },
    { }
};
MODULE_DEVICE_TABLE(pci, sfcx_ids);

static struct pci_driver sfcx_driver = {
    .name = "xenon_sfcx",
    .id_table = sfcx_ids,
    .probe = sfcx_probe,
    .remove = sfcx_remove,
    /* unbinding with holders would bypass the module reference that protects
     * open users; use rmmod instead */
    .driver.suppress_bind_attrs = true,
};
module_pci_driver(sfcx_driver);

MODULE_DESCRIPTION("Xenon flash controller (NAND via MTD, eMMC as block device)");
MODULE_LICENSE("GPL");
