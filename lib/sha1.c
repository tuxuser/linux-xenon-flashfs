/* sha1.c - public-domain style SHA-1 (RFC 3174), one-shot */
#include "sha1.h"

#ifdef __KERNEL__
#include <crypto/sha1.h>

/* the kernel already ships SHA-1 (CONFIG_CRYPTO_LIB_SHA1) */
void flashfs_sha1(const void *data, size_t len, uint8_t out[20])
{
    sha1(data, len, out);
}
#else

static uint32_t rol(uint32_t v, unsigned int n)
{
    return (v << n) | (v >> (32 - n));
}

static void block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80], a, b, c, d, e, t;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = rd32be(p + i * 4);
    for (; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k;

        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else             { f = b ^ c ^ d;                   k = 0xca62c1d6; }
        t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void flashfs_sha1(const void *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476,
                      0xc3d2e1f0 };
    const uint8_t *p = data;
    uint8_t tail[128];
    size_t n = len, rem, i;
    uint64_t bits = (uint64_t)len * 8;

    for (; n >= 64; n -= 64, p += 64)
        block(h, p);
    memset(tail, 0, sizeof(tail));
    memcpy(tail, p, n);
    tail[n] = 0x80;
    rem = n < 56 ? 64 : 128;
    for (i = 0; i < 8; i++)
        tail[rem - 1 - i] = (uint8_t)(bits >> (8 * i));
    block(h, tail);
    if (rem == 128)
        block(h, tail + 64);
    for (i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)h[i];
    }
}

#endif /* !__KERNEL__ */
