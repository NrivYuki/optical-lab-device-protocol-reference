/* SHA-256(FIPS 180-4)と HMAC-SHA-256(RFC 2104)。 */
#include "oldp_sha256.h"
#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL, 0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL, 0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL, 0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL, 0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL, 0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL, 0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL, 0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL, 0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void block(oldp_sha256_ctx *c, const uint8_t *p) {
    uint32_t w[64], a, b, d, e, f, g, h, cc, t1, t2;
    uint8_t i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void oldp_sha256_init(oldp_sha256_ctx *c) {
    static const uint32_t H0[8] = { 0x6a09e667UL, 0xbb67ae85UL, 0x3c6ef372UL, 0xa54ff53aUL, 0x510e527fUL, 0x9b05688cUL, 0x1f83d9abUL, 0x5be0cd19UL };
    memcpy(c->h, H0, sizeof H0);
    c->bits_lo = c->bits_hi = 0;
    c->used = 0;
}

void oldp_sha256_update(oldp_sha256_ctx *c, const uint8_t *data, size_t n) {
    while (n--) {
        c->buf[c->used++] = *data++;
        c->bits_lo += 8;
        if (c->bits_lo < 8) c->bits_hi++;
        if (c->used == 64) { block(c, c->buf); c->used = 0; }
    }
}

void oldp_sha256_final(oldp_sha256_ctx *c, uint8_t out[32]) {
    uint32_t lo = c->bits_lo, hi = c->bits_hi;
    uint8_t i, one = 0x80, zero = 0, len[8];
    oldp_sha256_update(c, &one, 1);
    while (c->used != 56) oldp_sha256_update(c, &zero, 1);
    for (i = 0; i < 4; i++) { len[i] = (uint8_t)(hi >> (24 - 8 * i)); len[4 + i] = (uint8_t)(lo >> (24 - 8 * i)); }
    oldp_sha256_update(c, len, 8);
    for (i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24); out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8); out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

void oldp_sha256(uint8_t out[32], const uint8_t *data, size_t n) {
    oldp_sha256_ctx c;
    oldp_sha256_init(&c);
    oldp_sha256_update(&c, data, n);
    oldp_sha256_final(&c, out);
}

void oldp_hmac_init(oldp_hmac_ctx *c, const uint8_t *key, size_t klen) {
    uint8_t k[64], ipad[64];
    uint8_t i;
    memset(k, 0, sizeof k);
    if (klen > 64) oldp_sha256(k, key, klen); else memcpy(k, key, klen);
    for (i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; c->okey[i] = k[i] ^ 0x5c; }
    oldp_sha256_init(&c->inner);
    oldp_sha256_update(&c->inner, ipad, 64);
}

void oldp_hmac_update(oldp_hmac_ctx *c, const uint8_t *data, size_t n) { oldp_sha256_update(&c->inner, data, n); }

void oldp_hmac_final(oldp_hmac_ctx *c, uint8_t out[32]) {
    uint8_t ih[32];
    oldp_sha256_ctx o;
    oldp_sha256_final(&c->inner, ih);
    oldp_sha256_init(&o);
    oldp_sha256_update(&o, c->okey, 64);
    oldp_sha256_update(&o, ih, 32);
    oldp_sha256_final(&o, out);
}
