/* mtd_dump - stream raw NAND pages (512 data + 16 spare) from an MTD char device.
 *
 *   mtd_dump /dev/mtd0ro START_PAGE NPAGES  > out      (528 bytes per page)
 *
 * Uses the MEMREAD ioctl with MTD_OPS_PLACE_OOB (spare returned as stored, no ECC
 * layout interpretation). Writes only to stdout; the device is opened read-only. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <mtd/mtd-abi.h>

int main(int argc, char **argv)
{
    uint8_t data[512], oob[16];
    unsigned long start, n, i;
    int fd;

    if (argc != 4) {
        fprintf(stderr, "usage: %s /dev/mtdNro START_PAGE NPAGES\n", argv[0]);
        return 2;
    }
    start = strtoul(argv[2], NULL, 0);
    n = strtoul(argv[3], NULL, 0);
    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    for (i = 0; i < n; i++) {
        struct mtd_read_req req = {
            .start = (start + i) * 512ull, .len = 512, .ooblen = 16,
            .usr_data = (uintptr_t)data, .usr_oob = (uintptr_t)oob,
            .mode = MTD_OPS_PLACE_OOB,
        };

        if (ioctl(fd, MEMREAD, &req) < 0) {
            fprintf(stderr, "page %lu: ", start + i);
            perror("MEMREAD");
            return 1;
        }
        if (fwrite(data, 1, 512, stdout) != 512 ||
            fwrite(oob, 1, 16, stdout) != 16) {
            perror("write");
            return 1;
        }
    }
    return 0;
}
