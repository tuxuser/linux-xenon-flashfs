/* mtd_nand - raw NAND primitives through the MTD char device ioctls, for the
 * live write tests (tests/live/nand_stage*.sh). NOTHING here is automatic: every
 * command does exactly one thing and returns its errno.
 *
 *   mtd_nand DEV erase BLOCK
 *   mtd_nand DEV blank BLOCK            all pages 0xFF (data + spare)?
 *   mtd_nand DEV wr PAGE SEED           program a patterned page (+ spare)
 *   mtd_nand DEV chk PAGE SEED          page == pattern, spare ECC valid?
 *   mtd_nand DEV wr_nooob PAGE SEED     data-only write (must be refused)
 *   mtd_nand DEV wr_badmark PAGE SEED   spare[5] != 0xff on a block's first page
 *                                       (must be refused)
 *   mtd_nand DEV root BLOCKS            newest filesystem root by spare sequence:
 *                                       prints "root=BLOCK seq=N" and all candidates
 *   mtd_nand DEV put_block BLOCK        program 32 raw records (528 B each) from stdin
 *                                       into an ERASED block (restoring from a backup)
 * Exit code: 0 ok, 1 negative result (blank/chk mismatch), errno on failure.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <mtd/mtd-abi.h>
#include "nand_hw.h"

#define PPB 32u

static void pattern(uint32_t page, uint32_t seed, uint8_t data[512], uint8_t oob[16])
{
    unsigned int i;

    for (i = 0; i < 512; i++)
        data[i] = (uint8_t)(seed * 31 + page * 13 + i * 7 + (i >> 3));
    memset(oob, 0, 16);
    oob[0] = page / PPB; oob[1] = (page / PPB) >> 8;       /* block number */
    oob[2] = 1; oob[5] = 0xff;                              /* "good block" */
    oob[8] = seed; oob[9] = page;                           /* test payload */
}

/* The controller serves a read of the page it just programmed from its page
 * buffer, so flush with a read of another page first. */
static int rd_page(int fd, uint32_t page, uint8_t raw[528])
{
    uint8_t dummy[528];
    struct mtd_read_req f = { .start = ((page + 1) % (1024 * PPB)) * 512ull, .len = 512,
        .ooblen = 16, .usr_data = (uintptr_t)dummy, .usr_oob = (uintptr_t)(dummy + 512),
        .mode = MTD_OPS_PLACE_OOB };

    ioctl(fd, MEMREAD, &f);
    struct mtd_read_req r = { .start = page * 512ull, .len = 512, .ooblen = 16,
        .usr_data = (uintptr_t)raw, .usr_oob = (uintptr_t)(raw + 512),
        .mode = MTD_OPS_PLACE_OOB };

    return ioctl(fd, MEMREAD, &r) < 0 ? -errno : 0;
}

int main(int argc, char **argv)
{
    uint8_t data[512], oob[16], raw[528];
    uint32_t a, seed = 0;
    int fd, ret = 0;
    const char *cmd;

    if (argc < 4) {
        fprintf(stderr, "usage: %s DEV CMD ARG [SEED]\n", argv[0]);
        return 2;
    }
    cmd = argv[2];
    a = strtoul(argv[3], NULL, 0);
    if (argc > 4)
        seed = strtoul(argv[4], NULL, 0);
    fd = open(argv[1], !strcmp(cmd, "blank") || !strcmp(cmd, "chk") || !strcmp(cmd, "root") ?
              O_RDONLY : O_RDWR);
    if (fd < 0) {
        perror("open");
        return 3;
    }
    if (!strcmp(cmd, "erase")) {
        struct erase_info_user e = { .start = a * PPB * 512, .length = PPB * 512 };

        if (ioctl(fd, MEMERASE, &e) < 0)
            ret = errno;
    } else if (!strcmp(cmd, "blank")) {
        uint32_t p;

        for (p = 0; p < PPB && !ret; p++) {
            int r = rd_page(fd, a * PPB + p, raw), i;

            if (r) { ret = -r; break; }
            for (i = 0; i < 528; i++)
                if (raw[i] != 0xff) { printf("block %u page %u byte %d = %02x\n", a, p, i, raw[i]); ret = 1; break; }
        }
    } else if (!strcmp(cmd, "chk")) {
        uint8_t ed[512], eo[16];
        int r = rd_page(fd, a, raw);

        pattern(a, seed, ed, eo);
        if (r) ret = -r;
        else if (memcmp(raw, ed, 512)) { printf("page %u: data mismatch\n", a); ret = 1; }
        else if (memcmp(raw + 512, eo, 12)) { printf("page %u: spare[0..11] mismatch\n", a); ret = 1; }
        else if (!nand_hw_ecc_ok(raw)) { printf("page %u: ECC invalid\n", a); ret = 1; }
    } else if (!strcmp(cmd, "wr") || !strcmp(cmd, "wr_nooob") || !strcmp(cmd, "wr_badmark")) {
        struct mtd_write_req w = { .start = a * 512ull, .len = 512, .ooblen = 16,
            .usr_data = (uintptr_t)data, .usr_oob = (uintptr_t)oob,
            .mode = MTD_OPS_PLACE_OOB };

        pattern(a, seed, data, oob);
        if (!strcmp(cmd, "wr_nooob")) { w.ooblen = 0; w.usr_oob = 0; }
        if (!strcmp(cmd, "wr_badmark")) oob[5] = 0x00;
        if (ioctl(fd, MEMWRITE, &w) < 0)
            ret = errno;
    } else if (!strcmp(cmd, "root")) {
        uint32_t b, best = 0, bseq = 0;

        for (b = 0; b < a; b++) {
            uint8_t sp[528];
            uint32_t seq;

            if (rd_page(fd, b * PPB, sp))
                continue;
            if (sp[517] != 0xff || (sp[524] & 0x3f) != 0x30)
                continue;
            seq = sp[514] | sp[515] << 8 | sp[516] << 16 | (uint32_t)sp[518] << 24;
            if (!seq)
                continue;
            printf("candidate block=%u seq=%u\n", b, seq);
            if (seq > bseq) { bseq = seq; best = b; }
        }
        printf("root=%u seq=%u\n", best, bseq);
    } else if (!strcmp(cmd, "put_block")) {
        uint32_t p;

        for (p = 0; p < PPB && !ret; p++) {
            uint8_t rec[528];
            struct mtd_write_req w = { .start = (a * PPB + p) * 512ull, .len = 512,
                .ooblen = 16, .usr_data = (uintptr_t)rec,
                .usr_oob = (uintptr_t)(rec + 512), .mode = MTD_OPS_PLACE_OOB };

            if (fread(rec, 1, sizeof(rec), stdin) != sizeof(rec)) { ret = 5; break; }
            if (ioctl(fd, MEMWRITE, &w) < 0)
                ret = errno;
        }
    } else {
        fprintf(stderr, "unknown command %s\n", cmd);
        ret = 2;
    }
    if (ret > 1)
        printf("%s %u: %s\n", cmd, a, strerror(ret));
    close(fd);
    return ret;
}
