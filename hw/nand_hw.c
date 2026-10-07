/* nand_hw.c - see nand_hw.h.
 *
 * The register sequences are those of libxenon's drivers/xenon_nand/xenon_sfcx.c
 * (sfcx_read_page / sfcx_write_page / sfcx_erase_block / sfcx_calcecc_ex), step for
 * step. Deliberate differences, and only these:
 *   - waits are bounded (libxenon spins forever) and failures are returned
 *     (libxenon prints them);
 *   - program/erase also treat the bad-block status bit as an error (libxenon
 *     prints it and carries on); reads keep libxenon's tolerance (BB/ECC bits are
 *     not errors for a raw read);
 *   - nand_hw_write_page generates the ECC itself (libxenon's callers call
 *     sfcx_calcecc() before sfcx_write_page());
 *   - after a program one OTHER page is read before the verify read: the SFC
 *     answers a read of the page it just programmed from its page buffer.
 */
#include "nand_hw.h"

#ifdef __KERNEL__
#include <linux/string.h>
#include <linux/unaligned.h>
#define LE32_GET(p) get_unaligned_le32(p)
#define LE32_PUT(v, p) put_unaligned_le32((v), (p))
#else
#include <string.h>
static inline uint32_t LE32_GET(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline void LE32_PUT(uint32_t v, uint8_t *p)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
#endif

#define R_CONFIG   0x00
#define R_STATUS   0x04
#define R_COMMAND  0x08
#define R_ADDRESS  0x0c
#define R_DATA     0x10

#define C_PAGE_BUF_TO_REG 0x00
#define C_REG_TO_PAGE_BUF 0x01
#define C_PHY_PAGE_TO_BUF 0x03
#define C_WRITE_PAGE      0x04
#define C_ERASE_BLOCK     0x05
#define C_UNLOCK_0        0x55
#define C_UNLOCK_1        0xaa

#define CFG_WP_EN         0x8u
#define ST_BUSY           0x001u
#define ST_ERR            0x8a2u  /* ILL_LOG|ADDR|RNP|WR: errors for any operation */
#define ST_ERR_WR         0x8e2u  /* ... + BB_ER (0x40): also an error when programming/erasing */

#define READ_WAIT_MS   100
#define WRITE_WAIT_MS  500
#define ERASE_WAIT_MS  2000

void nand_hw_ecc(const uint8_t raw[NAND_RAW_PAGE], uint8_t out[4])
{
    uint32_t val = 0, v = 0, i;

    for (i = 0; i < 0x1066; i++) {
        if (!(i & 31))
            v = ~LE32_GET(raw + (i >> 5) * 4);
        val ^= v & 1;
        v >>= 1;
        if (val & 1)
            val ^= 0x6954559;
        val >>= 1;
    }
    val = ~val;
    LE32_PUT(val << 6, out);
}

int nand_hw_ecc_ok(const uint8_t raw[NAND_RAW_PAGE])
{
    uint8_t c[4];

    nand_hw_ecc(raw, c);
    return (LE32_GET(raw + 524) >> 6) == (LE32_GET(c) >> 6);
}

static int wait_idle(nand_hw_t *hw, unsigned int ms, uint32_t *st)
{
    uint64_t end = hw->ops->now_ms(hw->ctx) + ms;

    for (;;) {
        *st = hw->ops->rd(hw->ctx, R_STATUS);
        if (!(*st & ST_BUSY))
            return 0;
        if (hw->ops->now_ms(hw->ctx) > end)
            return -ETIMEDOUT;
        if (hw->ops->relax)
            hw->ops->relax(hw->ctx);
    }
}

int nand_hw_read_page(nand_hw_t *hw, uint32_t page, uint8_t raw[NAND_RAW_PAGE])
{
    uint32_t st, i;
    int ret;

    hw->ops->wr(hw->ctx, R_STATUS, hw->ops->rd(hw->ctx, R_STATUS));
    hw->ops->wr(hw->ctx, R_ADDRESS, page * NAND_DATA_BYTES);
    hw->ops->wr(hw->ctx, R_COMMAND, C_PHY_PAGE_TO_BUF);
    ret = wait_idle(hw, READ_WAIT_MS, &st);
    hw->last_status = st;
    if (ret)
        return ret;
    hw->ops->wr(hw->ctx, R_ADDRESS, 0);
    for (i = 0; i < NAND_RAW_PAGE; i += 4) {
        hw->ops->wr(hw->ctx, R_COMMAND, C_PAGE_BUF_TO_REG);
        LE32_PUT(hw->ops->rd(hw->ctx, R_DATA), raw + i);
    }
    return (st & ST_ERR) ? -EIO : 0;
}

static void wp(nand_hw_t *hw, int on)
{
    uint32_t c = hw->ops->rd(hw->ctx, R_CONFIG);

    hw->ops->wr(hw->ctx, R_CONFIG, on ? c | CFG_WP_EN : c & ~CFG_WP_EN);
}

int nand_hw_write_page(nand_hw_t *hw, uint32_t page,
                       const uint8_t raw_in[NAND_RAW_PAGE])
{
    uint8_t raw[NAND_RAW_PAGE], back[NAND_RAW_PAGE], ecc[4];
    uint32_t st, i;
    int ret;

    if (!hw->writable)
        return -EOPNOTSUPP;
    if (page >= hw->blocks * hw->pages_per_block)
        return -EINVAL;
    memcpy(raw, raw_in, sizeof(raw));
    raw[524] &= 0x3f;                      /* only the 6 non-code bits survive */
    raw[525] = raw[526] = raw[527] = 0;
    nand_hw_ecc(raw, ecc);
    raw[524] |= ecc[0];
    memcpy(raw + 525, ecc + 1, 3);

    hw->ops->wr(hw->ctx, R_STATUS, 0xff);
    wp(hw, 1);
    hw->ops->wr(hw->ctx, R_ADDRESS, 0);
    for (i = 0; i < NAND_RAW_PAGE; i += 4) {
        hw->ops->wr(hw->ctx, R_DATA, LE32_GET(raw + i));
        hw->ops->wr(hw->ctx, R_COMMAND, C_REG_TO_PAGE_BUF);
    }
    hw->ops->wr(hw->ctx, R_ADDRESS, page * NAND_DATA_BYTES);
    hw->ops->wr(hw->ctx, R_COMMAND, C_UNLOCK_0);
    hw->ops->wr(hw->ctx, R_COMMAND, C_UNLOCK_1);
    ret = wait_idle(hw, WRITE_WAIT_MS, &st);
    if (!ret) {
        hw->ops->wr(hw->ctx, R_COMMAND, C_WRITE_PAGE);
        ret = wait_idle(hw, WRITE_WAIT_MS, &st);
    }
    hw->last_status = st;
    wp(hw, 0);
    if (ret)
        return ret;
    if (st & ST_ERR_WR)
        return -EIO;
    /* The controller answers a read of the page it just programmed from its
     * page buffer, i.e. with our own data. Read another page first so the
     * verify below sees what the flash really holds (found on hardware: an
     * out-of-spec re-program "verified" fine while the flash held old AND new). */
    nand_hw_read_page(hw, (page + 1) % (hw->blocks * hw->pages_per_block), back);
    ret = nand_hw_read_page(hw, page, back);
    if (ret)
        return ret;
    return memcmp(raw, back, sizeof(raw)) ? -EIO : 0;
}

int nand_hw_erase_block(nand_hw_t *hw, uint32_t block)
{
    uint32_t st;
    int ret;

    if (!hw->writable)
        return -EOPNOTSUPP;
    if (block >= hw->blocks)
        return -EINVAL;
    wp(hw, 1);
    hw->ops->wr(hw->ctx, R_STATUS, 0xff);
    hw->ops->wr(hw->ctx, R_ADDRESS,
                block * hw->pages_per_block * NAND_DATA_BYTES);
    ret = wait_idle(hw, ERASE_WAIT_MS, &st);
    if (!ret) {
        hw->ops->wr(hw->ctx, R_COMMAND, C_UNLOCK_1);
        hw->ops->wr(hw->ctx, R_COMMAND, C_UNLOCK_0);
        ret = wait_idle(hw, ERASE_WAIT_MS, &st);
    }
    if (!ret) {
        hw->ops->wr(hw->ctx, R_COMMAND, C_ERASE_BLOCK);
        ret = wait_idle(hw, ERASE_WAIT_MS, &st);
    }
    hw->last_status = st;
    hw->ops->wr(hw->ctx, R_STATUS, 0xff);
    wp(hw, 0);
    if (ret)
        return ret;
    return (st & ST_ERR_WR) ? -EIO : 0;
}

int nand_hw_block_marked_bad(nand_hw_t *hw, uint32_t block)
{
    uint8_t raw[NAND_RAW_PAGE];
    int ret = nand_hw_read_page(hw, block * hw->pages_per_block, raw);

    if (ret)
        return ret;
    return raw[512 + 5] != 0xff;
}
