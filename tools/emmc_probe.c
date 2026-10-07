#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

#define PCI_VENDOR 0x1414
#define PCI_DEVICE 0x580b
#define REG_TIMEOUT 0x04
#define REG_ARGUMENT 0x08
#define REG_COMMAND 0x0c
#define REG_RESPONSE 0x10
#define REG_PIO 0x20
#define REG_STATUS 0x24
#define REG_CONTROL 0x2c
#define REG_INTERRUPT 0x30

static volatile uint32_t *regs;

/* Linux sysfs BAR0 mapping; byte swap matches libxenon's SFCX accessors. */
static uint32_t mmio_read(unsigned int offset)
{
    uint32_t value = __builtin_bswap32(regs[offset / sizeof(uint32_t)]);
    __sync_synchronize();
    return value;
}

static void mmio_write(unsigned int offset, uint32_t value)
{
    __sync_synchronize();
    regs[offset / sizeof(uint32_t)] = __builtin_bswap32(value);
    __sync_synchronize();
}

/* libxenon reads the PIO FIFO in native endianness. */
static uint32_t pio_read_native(void)
{
    uint32_t value = regs[REG_PIO / sizeof(uint32_t)];
    __sync_synchronize();
    return value;
}

static uint64_t monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int read_hex(const char *path, unsigned int *value)
{
    char buf[32];
    char *end;
    unsigned long n;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t len;

    if (fd < 0)
        return -1;
    len = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (len <= 0)
        return -1;
    buf[len] = '\0';
    errno = 0;
    n = strtoul(buf, &end, 0);
    if (errno || end == buf)
        return -1;
    *value = (unsigned int)n;
    return 0;
}

static int find_resource(char *path, size_t path_len)
{
    DIR *dir = opendir("/sys/bus/pci/devices");
    struct dirent *item;
    char file[256];
    unsigned int vendor, device;

    if (!dir)
        return -1;
    while ((item = readdir(dir)) != NULL) {
        if (item->d_name[0] == '.')
            continue;
        if (snprintf(file, sizeof(file),
                     "/sys/bus/pci/devices/%s/vendor", item->d_name) >=
            (int)sizeof(file) || read_hex(file, &vendor) != 0 ||
            vendor != PCI_VENDOR)
            continue;
        if (snprintf(file, sizeof(file),
                     "/sys/bus/pci/devices/%s/device", item->d_name) >=
            (int)sizeof(file) || read_hex(file, &device) != 0 ||
            device != PCI_DEVICE)
            continue;
        if (snprintf(path, path_len,
                     "/sys/bus/pci/devices/%s/resource0", item->d_name) >=
            (int)path_len) {
            closedir(dir);
            return -1;
        }
        printf("SFCX PCI device: %s\n", item->d_name);
        closedir(dir);
        return 0;
    }
    closedir(dir);
    errno = ENODEV;
    return -1;
}

#define REG_DMA_DESC 0x58
#define ISTAT_ERROR 0x8000u
#define CMD_BASE 0x001a0000u      /* 48-bit response flags, as libxenon */
#define CMD_DATA 0x00200000u
#define CMD_PIO_READ 0x10u
#define EMMC_TIMEOUT 0x80200u     /* libxenon value; 0 would mean no timeout */

/* Mirrors libxenon emmc_cmd(): any non-zero interrupt status ends the wait,
 * error bits are fatal, the status is always acked, and data commands wait
 * for STATUS&3 == 0 before the FIFO is touched. */
static unsigned int wait_ms = 200;
static const uint8_t *g_wdata;   /* CMD24: push to FIFO after the command irq */
static int g_trace;

static int emmc_cmd(const char *name, uint8_t cmd, uint32_t arg, int pio_read,
                    uint32_t *resp0)
{
    uint32_t cmdreg = ((uint32_t)cmd << 24) | CMD_BASE;
    uint32_t istat = 0, status;
    uint64_t deadline;

    if (pio_read)
        cmdreg |= CMD_DATA | CMD_PIO_READ;
    if (g_wdata)
        cmdreg |= CMD_DATA | 0x80;
    istat = mmio_read(REG_INTERRUPT);
    if (istat)
        mmio_write(REG_INTERRUPT, istat);
    mmio_write(REG_TIMEOUT, EMMC_TIMEOUT);
    mmio_write(REG_ARGUMENT, arg);
    printf("%s: cmdreg=%08" PRIx32 " arg=%08" PRIx32 "\n", name, cmdreg, arg);
    mmio_write(REG_COMMAND, cmdreg);

    deadline = monotonic_ms() + wait_ms;
    do {
        istat = mmio_read(REG_INTERRUPT);
        if (istat)
            break;
    } while (monotonic_ms() < deadline);
    if (istat)
        mmio_write(REG_INTERRUPT, istat);
    printf("  irq=%08" PRIx32 " resp0=%08" PRIx32 "\n", istat,
           mmio_read(REG_RESPONSE));
    if (!istat) {
        errno = ETIMEDOUT;
        return -1;
    }
    if (istat >= ISTAT_ERROR) {
        errno = EIO;
        return -2;
    }
    if (g_wdata) {
        unsigned int i;

        if (g_trace)
            printf("  before FIFO push: irq=%08" PRIx32 " status=%08" PRIx32 "\n",
                   mmio_read(REG_INTERRUPT), mmio_read(REG_STATUS));
        for (i = 0; i < 512; i += 4) {
            uint32_t w;

            memcpy(&w, g_wdata + i, 4);
            regs[REG_PIO / 4] = w;
            __sync_synchronize();
        }
        if (g_trace)
            printf("  after FIFO push:  irq=%08" PRIx32 " status=%08" PRIx32 "\n",
                   mmio_read(REG_INTERRUPT), mmio_read(REG_STATUS));
    }
    if (pio_read || g_wdata) {
        deadline = monotonic_ms() + 200;
        while ((status = mmio_read(REG_STATUS)) & 3) {
            if (monotonic_ms() > deadline) {
                printf("  transfer timeout status=%08" PRIx32 "\n", status);
                errno = ETIMEDOUT;
                return -3;
            }
        }
    }
    if (resp0)
        *resp0 = mmio_read(REG_RESPONSE);
    return 0;
}

static void read_fifo(uint8_t data[512])
{
    unsigned int i;

    for (i = 0; i < 512; i += sizeof(uint32_t)) {
        uint32_t word = pio_read_native();
        memcpy(data + i, &word, sizeof(word));
    }
}

/* libxenon emmc_init() controller reset (without the 0xF0 tweak). */
static void ctl_reset(void)
{
    uint32_t c = mmio_read(REG_CONTROL);

    printf("control before reset: %08" PRIx32 "\n", c);
    mmio_write(REG_CONTROL, c & 0xfffffffau);
    usleep(10000);
    c = mmio_read(REG_CONTROL);
    mmio_write(REG_CONTROL, c & 0xffff00ffu);
    usleep(10000);
    c = mmio_read(REG_CONTROL);
    mmio_write(REG_CONTROL, c | 5);
    usleep(50000);
    mmio_write(REG_INTERRUPT, mmio_read(REG_INTERRUPT));
    printf("control after reset: %08" PRIx32 " status=%08" PRIx32 "\n",
           mmio_read(REG_CONTROL), mmio_read(REG_STATUS));
}

/* Debug: watch interrupt/status registers for a while after a command. */
static void observe(unsigned int ms)
{
    uint64_t end = monotonic_ms() + ms, t0 = monotonic_ms();

    while (monotonic_ms() < end) {
        uint32_t i = mmio_read(REG_INTERRUPT);

        printf("  t+%" PRIu64 "ms irq=%08" PRIx32 " status=%08" PRIx32 " resp0=%08" PRIx32 "\n",
               monotonic_ms() - t0, i, mmio_read(REG_STATUS), mmio_read(REG_RESPONSE));
        if (i)
            mmio_write(REG_INTERRUPT, i);
        usleep(100000);
    }
}

static int cmd_status(void)
{
    uint32_t r = 0;
    int ret = emmc_cmd("CMD13 SEND_STATUS", 13, 0xffffffffu, 0, &r);

    if (!ret)
        printf("card state=%u (4 = transfer)\n", (r >> 9) & 0xf);
    return ret;
}

static int cmd_ext_csd(void)
{
    uint8_t data[512];
    uint32_t sectors;
    int ret = emmc_cmd("CMD8 SEND_EXT_CSD", 8, 0, 1, NULL);

    if (ret)
        return ret;
    read_fifo(data);
    sectors = (uint32_t)data[212] | (uint32_t)data[213] << 8 |
              (uint32_t)data[214] << 16 | (uint32_t)data[215] << 24;
    printf("EXT_CSD sector count=%" PRIu32 " (%" PRIu64 " MiB)\n", sectors,
           (uint64_t)sectors / 2048);
    printf("EXT_CSD: rev=%u(b192) bus_width=%u(b183) hs_timing=%u(b185) device_type=%02x(b196)\n"
           "         part_config=%02x(b179) boot_size_mult=%u(b226) erase_grp_def=%u(b175) "
           "cache_ctrl=%u(b33) cache_size=%u KiB(b249..) cmdq=%u(b15) pwr_cl=%u(b187) "
           "rst_n=%u(b162) boot_bus=%02x(b177)\n",
           data[192], data[183], data[185], data[196], data[179], data[226], data[175],
           data[33], data[249] | data[250] << 8 | data[251] << 16 | data[252] << 24,
           data[15], data[187], data[162], data[177]);
    return sectors ? 0 : -1;
}

/* Interrupt timeline of one single-block read: which SDHCI status bits does the
 * hardware actually raise, and when? Nothing is acked until the end. */
static int cmd_trace(uint32_t lba)
{
    struct { uint64_t us; uint32_t irq, present; } ev[64];
    unsigned int n = 0, i;
    struct timespec t0, t;
    uint32_t last_irq = 0xffffffff, last_pr = 0xffffffff, istat;
    uint8_t data[512];

    istat = mmio_read(REG_INTERRUPT);
    if (istat)
        mmio_write(REG_INTERRUPT, istat);
    mmio_write(REG_TIMEOUT, 0x10200);          /* block size 512, count 1 */
    mmio_write(REG_ARGUMENT, lba);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    mmio_write(REG_COMMAND, 0x113a0010u);
    for (;;) {
        uint32_t irq = mmio_read(REG_INTERRUPT), pr = mmio_read(REG_STATUS);

        clock_gettime(CLOCK_MONOTONIC, &t);
        if ((irq != last_irq || pr != last_pr) && n < 64) {
            ev[n].us = (uint64_t)(t.tv_sec - t0.tv_sec) * 1000000 +
                       (t.tv_nsec - t0.tv_nsec) / 1000;
            ev[n].irq = irq;
            ev[n].present = pr;
            n++;
            last_irq = irq;
            last_pr = pr;
        }
        if ((uint64_t)(t.tv_sec - t0.tv_sec) * 1000000 +
            (t.tv_nsec - t0.tv_nsec) / 1000 > 20000)
            break;
    }
    for (i = 0; i < n; i++)
        printf("  +%6lluus  int_status=%08x  present=%08x\n",
               (unsigned long long)ev[i].us, ev[i].irq, ev[i].present);
    mmio_write(REG_INTERRUPT, mmio_read(REG_INTERRUPT));
    read_fifo(data);                            /* drain the block */
    mmio_write(REG_INTERRUPT, mmio_read(REG_INTERRUPT));
    printf("after drain: int_status=%08x present=%08x\n",
           mmio_read(REG_INTERRUPT), mmio_read(REG_STATUS));
    return 0;
}

/* Raw command timeline: --raw CMDREG ARG (cmdreg as written to 0x0C, i.e.
 * index<<24 | flags<<16 | transfer mode). Resets nothing, acks at the end. */
static int cmd_raw(uint32_t cmdreg, uint32_t arg)
{
    struct { uint64_t us; uint32_t irq, present; } ev[64];
    unsigned int n = 0, i;
    struct timespec t0, t;
    uint32_t last_irq = 0xffffffff, last_pr = 0xffffffff, istat;

    istat = mmio_read(REG_INTERRUPT);
    if (istat)
        mmio_write(REG_INTERRUPT, istat);
    mmio_write(REG_TIMEOUT, 0x10200);
    mmio_write(REG_ARGUMENT, arg);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    mmio_write(REG_COMMAND, cmdreg);
    for (;;) {
        uint32_t irq = mmio_read(REG_INTERRUPT), pr = mmio_read(REG_STATUS);
        uint64_t us;

        clock_gettime(CLOCK_MONOTONIC, &t);
        us = (uint64_t)(t.tv_sec - t0.tv_sec) * 1000000 + (t.tv_nsec - t0.tv_nsec) / 1000;
        if ((irq != last_irq || pr != last_pr) && n < 64) {
            ev[n].us = us;
            ev[n].irq = irq;
            ev[n].present = pr;
            n++;
            last_irq = irq;
            last_pr = pr;
        }
        if (us > 50000)
            break;
    }
    for (i = 0; i < n; i++)
        printf("  +%6lluus  int_status=%08x  present=%08x\n",
               (unsigned long long)ev[i].us, ev[i].irq, ev[i].present);
    printf("resp0=%08x\n", mmio_read(REG_RESPONSE));
    mmio_write(REG_INTERRUPT, mmio_read(REG_INTERRUPT));
    return 0;
}

/* Re-initialisation experiment, userspace only: CMD0, CMD1 until ready, then
 * CMD2 with different response flag choices and CMD3. */
static uint32_t raw_cmd(uint32_t cmdreg, uint32_t arg, unsigned int wait_us, uint32_t *irq_out, uint32_t resp[4])
{
    struct timespec t0, t;
    uint32_t irq = 0, istat = mmio_read(REG_INTERRUPT);

    if (istat)
        mmio_write(REG_INTERRUPT, istat);
    mmio_write(REG_TIMEOUT, 0x10200);
    mmio_write(REG_ARGUMENT, arg);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    mmio_write(REG_COMMAND, cmdreg);
    do {
        irq = mmio_read(REG_INTERRUPT);
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (irq)
            break;
    } while ((uint64_t)(t.tv_sec - t0.tv_sec) * 1000000 + (t.tv_nsec - t0.tv_nsec) / 1000 < wait_us);
    mmio_write(REG_INTERRUPT, mmio_read(REG_INTERRUPT));
    if (resp)
        for (istat = 0; istat < 4; istat++)
            resp[istat] = mmio_read(REG_RESPONSE + istat * 4);
    *irq_out = irq;
    return irq;
}

static int cmd_reinit(void)
{
    uint32_t irq, r[4];
    unsigned int i;
    static const uint32_t flags2[] = { 0x09, 0x01, 0x0b, 0x19, 0x1b };

    raw_cmd(0x00000000u, 0, 50000, &irq, r);
    printf("CMD0 irq=%08x\n", irq);
    for (i = 0; i < 200; i++) {
        raw_cmd(0x01020000u, 0x40ff8000u, 50000, &irq, r);
        if (irq == 1 && (r[0] & 0x80000000u))
            break;
        usleep(10000);
    }
    printf("CMD1 ready after %u polls: irq=%08x ocr=%08x\n", i, irq, r[0]);
    for (i = 0; i < sizeof(flags2) / sizeof(flags2[0]); i++) {
        raw_cmd(0x02000000u | (flags2[i] << 16), 0, 50000, &irq, r);
        printf("CMD2 flags=%02x irq=%08x resp=%08x %08x %08x %08x\n", flags2[i], irq, r[0], r[1], r[2], r[3]);
        if (irq == 1 && r[0] != 0xffffffffu)
            break;
    }
    raw_cmd(0x031a0000u, 0x00010000u, 50000, &irq, r);
    printf("CMD3 irq=%08x resp0=%08x\n", irq, r[0]);
    return 0;
}

/* Read the real CID and CSD of the selected card: deselect (CMD7 RCA 0 ->
 * stand-by), CMD9/CMD10 (R2), select again. Leaves the card in transfer state. */
static int cmd_readcid(void)
{
    uint32_t irq, r[4], st = 0;
    int ret;

    raw_cmd(0x07000000u, 0, 50000, &irq, r);            /* deselect, no response */
    printf("CMD7 deselect irq=%08x\n", irq);
    raw_cmd(0x090a0000u | 0x0100000u * 0, 0xffff0000u, 50000, &irq, r);
    printf("CMD9  CSD irq=%08x resp=%08x %08x %08x %08x\n", irq, r[3], r[2], r[1], r[0]);
    raw_cmd(0x0a090000u, 0xffff0000u, 50000, &irq, r);
    printf("CMD10 CID irq=%08x resp=%08x %08x %08x %08x\n", irq, r[3], r[2], r[1], r[0]);
    raw_cmd(0x071b0000u, 0xffff0000u, 200000, &irq, r);   /* select again (R1b) */
    printf("CMD7 select irq=%08x resp0=%08x\n", irq, r[0]);
    ret = emmc_cmd("CMD13 SEND_STATUS", 13, 0xffffffffu, 0, &st);
    printf("CMD13 -> %d state=%u\n", ret, (st >> 9) & 0xf);
    return 0;
}

static int cmd_stop(void)
{
    int ret = emmc_cmd("CMD12 STOP_TRANSMISSION", 12, 0, 0, NULL);

    cmd_status();
    return ret;
}

/* Safe write test: read a sector, then write the identical data back, right
 * after each other (the sequence that failed in the kernel driver). */
static int cmd_rewrite(uint32_t lba)
{
    uint8_t data[512];
    uint32_t r = 0;
    int ret = emmc_cmd("CMD17 READ", 17, lba, 1, NULL);

    if (ret)
        return ret;
    read_fifo(data);
    g_trace = 1;
    g_wdata = data;
    ret = emmc_cmd("CMD24 WRITE (same data)", 24, lba, 0, NULL);
    g_wdata = NULL;
    printf("write cmd -> %d\n", ret);
    if (ret)
        return ret;
    do {
        usleep(1000);
        ret = emmc_cmd("CMD13", 13, 0xffffffffu, 0, &r);
    } while (!ret && (((r >> 9) & 0xf) != 4 || !(r & 0x100)) && --wait_ms);
    printf("card status %08" PRIx32 "\n", r);
    return ret;
}

static int cmd_read_page(uint32_t lba)
{
    uint8_t data[512];
    unsigned int i;
    int ret = emmc_cmd("CMD17 READ_SINGLE_BLOCK", 17, lba, 1, NULL);

    if (ret)
        return ret;
    read_fifo(data);
    printf("block %" PRIu32 " first 32 bytes:", lba);
    for (i = 0; i < 32; i++)
        printf(" %02x", data[i]);
    putchar('\n');
    return 0;
}

/* sysfs resourceN maps whole pages. With 64K pages the 1K SFC BAR at
 * 0xea00c000 is mapped from 0xea000000 (the south bridge, incl. its IRQ
 * routing/mask registers), so the offset inside the page must be added. */
static volatile uint32_t *map_bar(const char *resource, int fd, int prot,
                                  void **mapping, size_t *maplen)
{
    char path[300], line[128];
    unsigned long long start = 0;
    size_t pg = (size_t)sysconf(_SC_PAGESIZE), off;
    size_t n = strlen(resource);
    FILE *f;

    if (n < 1 || n >= sizeof(path))
        return NULL;
    memcpy(path, resource, n - 1); /* ".../resource0" -> ".../resource" */
    path[n - 1] = '\0';
    f = fopen(path, "r");
    if (!f)
        return NULL;
    if (!fgets(line, sizeof(line), f) || sscanf(line, "%llx", &start) != 1) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    off = (size_t)(start & (pg - 1));
    if (off + 0x100 > pg)
        return NULL;
    *maplen = pg;
    *mapping = mmap(NULL, pg, prot, MAP_SHARED, fd, 0);
    if (*mapping == MAP_FAILED)
        return NULL;
    printf("BAR0 phys %#llx, page offset %#zx\n", start, off);
    return (volatile uint32_t *)((char *)*mapping + off);
}

static int probe_status(void)
{
    static const unsigned int offsets[] = { 0x00, 0x04, 0x24, 0x2c, 0x30, 0x3c, 0xf0, 0xfc };
    char resource[256];
    struct stat st;
    void *mapping;
    size_t maplen;
    int fd, i;

    if (find_resource(resource, sizeof(resource)) != 0) {
        perror("find SFCX BAR0");
        return EXIT_FAILURE;
    }
    fd = open(resource, O_RDONLY | O_SYNC | O_CLOEXEC);
    if (fd < 0) {
        perror(resource);
        return EXIT_FAILURE;
    }
    if (fstat(fd, &st) != 0 || st.st_size < 0xf4) {
        fprintf(stderr, "SFCX BAR0 is shorter than 0xf4 bytes\n");
        close(fd);
        return EXIT_FAILURE;
    }
    regs = map_bar(resource, fd, PROT_READ, &mapping, &maplen);
    if (!regs) {
        perror("mmap SFCX BAR0");
        close(fd);
        return EXIT_FAILURE;
    }
    puts("SFCX BAR0 full dump (read-only, skips FIFO 0x20):");
    for (i = 0; i < 0x100; i += 4) {
        if ((i & 0x1f) == 0)
            printf("\n[%02x]", i);
        if (i == REG_PIO)
            printf(" ........");
        else
            printf(" %08" PRIx32, mmio_read(i));
    }
    putchar('\n');
    puts("SFCX BAR0 registers (read-only):");
    for (i = 0; i < (int)(sizeof(offsets) / sizeof(offsets[0])); i++)
        printf("  [%02x]=%08" PRIx32 "\n", offsets[i], mmio_read(offsets[i]));
    munmap(mapping, maplen);
    close(fd);
    return EXIT_SUCCESS;
}


int main(int argc, char **argv)
{
    char resource[256];
    struct stat st;
    void *mapping;
    size_t maplen;
    int fd, ret = EXIT_FAILURE;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (geteuid() != 0) {
        fprintf(stderr, "Run as root to map the SFCX PCI BAR.\n");
        return EXIT_FAILURE;
    }
    if (argc == 2 && strcmp(argv[1], "--status") == 0)
        return probe_status();
    if (argc < 2 || (strcmp(argv[1], "--cmd13") && strcmp(argv[1], "--cmd12") && strcmp(argv[1], "--trace") && strcmp(argv[1], "--raw") && strcmp(argv[1], "--reinit") && strcmp(argv[1], "--readcid") && strcmp(argv[1], "--rewrite") && strcmp(argv[1], "--reset") && strcmp(argv[1], "--ext-csd") &&
                     strcmp(argv[1], "--read-page"))) {
        fprintf(stderr, "Usage: emmc-probe --status|--reset|--cmd13|--ext-csd|--read-page [lba]\n");
        return EXIT_FAILURE;
    }
    if (find_resource(resource, sizeof(resource)) != 0) {
        perror("find SFCX PCI BAR");
        return EXIT_FAILURE;
    }
    fd = open(resource, O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) {
        perror(resource);
        return EXIT_FAILURE;
    }
    if (fstat(fd, &st) != 0 || st.st_size < 0xf4) {
        fprintf(stderr, "SFCX BAR is shorter than 0xf4 bytes\n");
        close(fd);
        return EXIT_FAILURE;
    }
    regs = map_bar(resource, fd, PROT_READ | PROT_WRITE, &mapping, &maplen);
    if (!regs) {
        perror("mmap SFCX BAR");
        close(fd);
        return EXIT_FAILURE;
    }
    if (getenv("WAIT_MS"))
        wait_ms = (unsigned int)strtoul(getenv("WAIT_MS"), NULL, 0);

    if (getenv("OBSERVE_MS"))
        wait_ms = 1, ret = emmc_cmd("CMD13", 13, 0xffffffffu, 0, NULL), observe((unsigned int)strtoul(getenv("OBSERVE_MS"), NULL, 0));
    else if (!strcmp(argv[1], "--reset")) {
        ctl_reset();
        ret = cmd_status();
    } else if (!strcmp(argv[1], "--cmd13"))
        ret = cmd_status();
    else if (!strcmp(argv[1], "--cmd12"))
        ret = cmd_stop();
    else if (!strcmp(argv[1], "--readcid"))
        ret = cmd_readcid();
    else if (!strcmp(argv[1], "--reinit"))
        ret = cmd_reinit();
    else if (!strcmp(argv[1], "--raw") && argc > 3)
        ret = cmd_raw((uint32_t)strtoul(argv[2], NULL, 0), (uint32_t)strtoul(argv[3], NULL, 0));
    else if (!strcmp(argv[1], "--trace"))
        ret = cmd_trace(argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 0);
    else if (!strcmp(argv[1], "--rewrite"))
        ret = cmd_rewrite(argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 0);
    else if (!strcmp(argv[1], "--ext-csd"))
        ret = cmd_ext_csd();
    else
        ret = cmd_read_page(argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 0);
    ret = ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    munmap(mapping, maplen);
    close(fd);
    return ret;
}
