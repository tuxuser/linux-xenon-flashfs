/* emmc_hw.c - see emmc_hw.h. Command flow mirrors libxenon's emmc_cmd(). */
#include "emmc_hw.h"

#define REG_TIMEOUT   0x04
#define REG_ARGUMENT  0x08
#define REG_COMMAND   0x0c
#define REG_RESPONSE  0x10
#define REG_STATUS    0x24
#define REG_INTERRUPT 0x30

#define CMD_BASE      0x001a0000u /* 48-bit response flags */
#define CMD_DATA      0x00200000u
#define CMD_R2        0x00090000u /* 136-bit response + CRC check */
#define CMD_R1B       0x001b0000u /* 48-bit + busy */
#define CMD_PIO_READ  0x00000010u
#define CMD_PIO_WRITE 0x00000080u
#define CMD_TIMEOUT   0x00080200u /* 0 would mean no hardware timeout */
#define ISTAT_ERROR   0x8000u
#define ISTAT_CMD_DONE 0x01u
#define ISTAT_PIO_READY 0x10u /* write: ready for the FIFO fill */
#define WAIT_MS       200

#define MMC_SELECT_CARD   7
#define MMC_SEND_EXT_CSD  8
#define MMC_SEND_CSD      9
#define MMC_SEND_CID      10
#define MMC_SEND_STATUS   13
#define MMC_READ_SINGLE   17
#define MMC_WRITE_BLOCK   24
#define MMC_STOP_TRANSMISSION 12
#define PROGRAM_WAIT_MS   3000   /* eMMC write busy can be long (GC/merge) */
#define MMC_STATE_TRAN    4

/* Wait until neither the command nor the data line is busy (R1b busy, a
 * programming sector, ...). */
static int wait_idle(emmc_hw_t *hw, unsigned int ms)
{
    uint64_t deadline = hw->ops->now_ms(hw->ctx) + ms;

    while (hw->ops->rd(hw->ctx, REG_STATUS) & 3) {
        if (hw->ops->now_ms(hw->ctx) > deadline)
            return -1;
        hw->ops->relax(hw->ctx);
    }
    return 0;
}

static int hw_cmd_f(emmc_hw_t *hw, uint8_t cmd, uint32_t flags, uint32_t arg,
                    int pio_read, const uint8_t *wdata, uint32_t resp[4])
{
    const struct emmc_hw_ops *o = hw->ops;
    uint32_t cmdreg = ((uint32_t)cmd << 24) | flags;
    uint32_t istat, status, seen, need;
    uint64_t deadline;

    if (wait_idle(hw, PROGRAM_WAIT_MS)) {
        o->log(hw->ctx, "emmc: controller busy before cmd", cmd, 0);
        return -1;
    }

    if (pio_read)
        cmdreg |= CMD_DATA | CMD_PIO_READ;
    if (wdata)
        cmdreg |= CMD_DATA | CMD_PIO_WRITE;
    istat = o->rd(hw->ctx, REG_INTERRUPT);
    if (istat)
        o->wr(hw->ctx, REG_INTERRUPT, istat);
    o->wr(hw->ctx, REG_TIMEOUT, CMD_TIMEOUT);
    o->wr(hw->ctx, REG_ARGUMENT, arg);
    o->wr(hw->ctx, REG_COMMAND, cmdreg);

    /* Interrupt bits arrive one at a time (CMD24: 0x10 write-ready, then
     * 0x01 command done). Accumulate, ack as we go, and only continue when
     * every required bit was seen - "first non-zero" races with the response
     * register and the FIFO and left the card stuck in receive state. */
    seen = 0;
    need = wdata ? (ISTAT_CMD_DONE | ISTAT_PIO_READY) : ISTAT_CMD_DONE;
    deadline = o->now_ms(hw->ctx) + WAIT_MS;
    for (;;) {
        istat = o->rd(hw->ctx, REG_INTERRUPT);
        if (istat) {
            o->wr(hw->ctx, REG_INTERRUPT, istat);
            seen |= istat;
            if (seen >= ISTAT_ERROR || (seen & need) == need)
                break;
        }
        if (o->now_ms(hw->ctx) > deadline) {
            o->log(hw->ctx, "emmc: cmd timeout", cmd, seen);
            return -1;
        }
        o->relax(hw->ctx);
    }
    if (seen >= ISTAT_ERROR) {
        o->log(hw->ctx, "emmc: cmd error istatus", cmd, seen);
        return -2;
    }
    if (wdata) {
        unsigned int i;

        for (i = 0; i < 512; i += 4) {
            uint32_t w;

            memcpy(&w, wdata + i, sizeof(w));
            o->pio_write(hw->ctx, w);   /* native endianness, like read */
        }
    }
    if (pio_read || wdata) {
        /* for writes the busy bit stays set until the card finished programming */
        deadline = o->now_ms(hw->ctx) + (wdata ? PROGRAM_WAIT_MS : WAIT_MS);
        while ((status = o->rd(hw->ctx, REG_STATUS)) & 3) {
            if (o->now_ms(hw->ctx) > deadline) {
                o->log(hw->ctx, "emmc: transfer timeout", cmd, status);
                return -3;
            }
            o->relax(hw->ctx);
        }
    }
    if (pio_read || wdata) {            /* drop late completion bits */
        istat = o->rd(hw->ctx, REG_INTERRUPT);
        if (istat)
            o->wr(hw->ctx, REG_INTERRUPT, istat);
    }
    if (resp) {
        unsigned int i;

        for (i = 0; i < 4; i++)
            resp[i] = o->rd(hw->ctx, REG_RESPONSE + i * 4);
    }
    return 0;
}

static int hw_cmd_x(emmc_hw_t *hw, uint8_t cmd, uint32_t arg, int pio_read,
                    const uint8_t *wdata, uint32_t *resp0)
{
    uint32_t r[4];
    int ret = hw_cmd_f(hw, cmd, CMD_BASE, arg, pio_read, wdata, r);

    if (!ret && resp0)
        *resp0 = r[0];
    return ret;
}

static int hw_cmd(emmc_hw_t *hw, uint8_t cmd, uint32_t arg, int pio_read,
                  uint32_t *resp0)
{
    return hw_cmd_x(hw, cmd, arg, pio_read, NULL, resp0);
}

static void read_fifo(emmc_hw_t *hw, uint8_t out[512])
{
    unsigned int i;

    for (i = 0; i < 512; i += 4) {
        uint32_t w = hw->ops->pio(hw->ctx);
        memcpy(out + i, &w, sizeof(w));
    }
}

int emmc_hw_init(emmc_hw_t *hw)
{
    uint8_t ext[512];
    uint32_t resp = 0;
    int ret;

    hw->ready = 0;
    hw->sectors = 0;
    ret = hw_cmd(hw, MMC_SEND_STATUS, 0xffffffffu, 0, &resp);
    if (ret || ((resp >> 9) & 0xf) != MMC_STATE_TRAN) {
        /* Not selected: select with the KSB's RCA (0xFFFF). */
        ret = hw_cmd(hw, MMC_SELECT_CARD, 0xffff0000u, 0, NULL);
        if (ret)
            return ret;
    }
    ret = hw_cmd(hw, MMC_SEND_EXT_CSD, 0, 1, NULL);
    if (ret)
        return ret;
    read_fifo(hw, ext);
    hw->sectors = (uint32_t)ext[212] | (uint32_t)ext[213] << 8 |
                  (uint32_t)ext[214] << 16 | (uint32_t)ext[215] << 24;
    if (!hw->sectors)
        return -4;
    hw->ready = 1;
    return 0;
}

static int read_sector(emmc_hw_t *hw, uint32_t lba, uint8_t out[512])
{
    int ret, tries;

    for (tries = 0; tries < 3; tries++) {
        ret = hw_cmd(hw, MMC_READ_SINGLE, lba, 1, NULL);
        if (!ret) {
            read_fifo(hw, out);
            return 0;
        }
    }
    return ret;
}

int emmc_hw_read(void *context, uint64_t offset, void *buffer, size_t length)
{
    emmc_hw_t *hw = context;
    uint8_t *out = buffer;
    uint8_t sec[512];

    if (!hw->ready || offset / 512 + (length + 511) / 512 > hw->sectors)
        return -1;
    while (length) {
        uint32_t in = (uint32_t)(offset & 511);
        size_t n = 512 - in;

        if (n > length)
            n = length;
        if (in == 0 && n == 512) {
            if (read_sector(hw, (uint32_t)(offset >> 9), out))
                return -1;
        } else {
            if (read_sector(hw, (uint32_t)(offset >> 9), sec))
                return -1;
            memcpy(out, sec + in, n);
        }
        out += n;
        offset += n;
        length -= n;
    }
    return 0;
}

/* After CMD24 the card is busy programming: poll CMD13 until it is back in
 * transfer state and READY_FOR_DATA (bit 8) is set. */
static int wait_programmed(emmc_hw_t *hw)
{
    uint64_t deadline = hw->ops->now_ms(hw->ctx) + PROGRAM_WAIT_MS;
    uint32_t resp;

    for (;;) {
        if (!hw_cmd(hw, MMC_SEND_STATUS, 0xffffffffu, 0, &resp) &&
            (resp & 0x100) && ((resp >> 9) & 0xf) == MMC_STATE_TRAN)
            return resp & 0xffff0000u ? -5 : 0; /* error bits in card status */
        if (hw->ops->now_ms(hw->ctx) > deadline)
            return -6;
        hw->ops->relax(hw->ctx);
    }
}

int emmc_hw_write_sector(emmc_hw_t *hw, uint32_t lba, const uint8_t data[512])
{
    int ret;

    if (!hw->ready || lba >= hw->sectors || !hw->ops->pio_write)
        return -1;
    ret = hw_cmd_x(hw, MMC_WRITE_BLOCK, lba, 0, data, NULL);
    if (ret) {
        /* CMD12 is only legal while the card still waits for data (rcv, 6);
         * if it is programming (7) just wait for it to finish. */
        uint32_t r = 0;

        if (!hw_cmd(hw, MMC_SEND_STATUS, 0xffffffffu, 0, &r) &&
            ((r >> 9) & 0xf) == 6)
            hw_cmd(hw, MMC_STOP_TRANSMISSION, 0, 0, NULL);
        wait_programmed(hw);
        return ret;
    }
    return wait_programmed(hw);
}

int emmc_hw_write(emmc_hw_t *hw, uint64_t offset, const void *buffer,
                  size_t length)
{
    const uint8_t *in = buffer;
    uint8_t sec[512];

    if (!hw->ready || offset / 512 + (length + 511) / 512 > hw->sectors)
        return -1;
    while (length) {
        uint32_t off = (uint32_t)(offset & 511);
        size_t n = 512 - off;
        const uint8_t *src = in;
        int ret;

        if (n > length)
            n = length;
        if (off || n != 512) {          /* partial sector: read-modify-write */
            if (emmc_hw_read(hw, offset & ~511ULL, sec, 512))
                return -1;
            memcpy(sec + off, in, n);
            src = sec;
        }
        ret = emmc_hw_write_sector(hw, (uint32_t)(offset >> 9), src);
        if (ret)
            return ret;
        in += n;
        offset += n;
        length -= n;
    }
    return 0;
}

int emmc_hw_read_ext_csd(emmc_hw_t *hw, uint8_t out[512])
{
    int ret = hw_cmd(hw, MMC_SEND_EXT_CSD, 0, 1, NULL);

    if (ret)
        return ret;
    read_fifo(hw, out);
    return 0;
}

int emmc_hw_card_status(emmc_hw_t *hw, uint32_t *status)
{
    return hw_cmd(hw, MMC_SEND_STATUS, 0xffffffffu, 0, status);
}

/*
 * CID and CSD can only be read from a card in stand-by state, so the card is
 * de-selected for the duration (CMD7 with RCA 0), CMD9/CMD10 are issued and the
 * card is selected again. The caller must hold off all other I/O meanwhile. The
 * card's RCA is the one the KSB assigned (0xFFFF). Words are the controller's
 * four response registers, RESP3..RESP0 (the CRC byte is not included).
 */
int emmc_hw_read_ident(emmc_hw_t *hw, uint32_t cid[4], uint32_t csd[4])
{
    uint32_t rca = EMMC_CARD_RCA << 16;
    uint32_t st = 0;
    int ret, ret2, i;

    ret = hw_cmd_f(hw, MMC_SELECT_CARD, 0, 0, 0, NULL, NULL);   /* stand-by */
    if (ret)
        return ret;
    ret = hw_cmd_f(hw, MMC_SEND_CSD, CMD_R2, rca, 0, NULL, csd);
    if (!ret)
        ret = hw_cmd_f(hw, MMC_SEND_CID, CMD_R2, rca, 0, NULL, cid);
    ret2 = hw_cmd_f(hw, MMC_SELECT_CARD, CMD_R1B, rca, 0, NULL, NULL);
    if (!ret2) {
        for (i = 0; i < 20; i++) {                   /* back in transfer state? */
            if (!emmc_hw_card_status(hw, &st) &&
                ((st >> 9) & 0xf) == MMC_STATE_TRAN)
                break;
            hw->ops->relax(hw->ctx);
        }
        if (i == 20)
            ret2 = -7;
    }
    return ret ? ret : ret2;
}
