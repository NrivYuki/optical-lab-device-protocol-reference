/* X25519(RFC 7748)。16 ビットの CPU で小さく速く動くように、体の要素を 16 ビットの語 16 個で持つ。
 * 値は 2^256 未満で持ち、2^256 ≡ 38 (mod 2^255-19) で畳む。秘密の値で分岐・番地を変えない。 */
#include "oldp_x25519.h"

typedef uint16_t fe[16];

/* c(前の演算の繰り上がり)を 38 倍して足し込む。残った繰り上がりは r が小さくなっているので r[0] に収まる */
static void fold(fe r, uint32_t c) {
    uint8_t i;
    c *= 38;
    for (i = 0; i < 16; i++) { c += r[i]; r[i] = (uint16_t)c; c >>= 16; }
    r[0] = (uint16_t)(r[0] + (uint16_t)c * 38u);
}

static void fe_add(fe r, const fe a, const fe b) {
    uint32_t c = 0;
    uint8_t i;
    for (i = 0; i < 16; i++) { c += (uint32_t)a[i] + b[i]; r[i] = (uint16_t)c; c >>= 16; }
    fold(r, c);
}

static void fe_sub(fe r, const fe a, const fe b) {
    uint32_t t, borrow = 0;
    uint8_t i;
    for (i = 0; i < 16; i++) { t = (uint32_t)a[i] - b[i] - borrow; r[i] = (uint16_t)t; borrow = (t >> 16) & 1; }
    /* 借りたぶん(2^256 ≡ 38)を引く */
    borrow *= 38;
    for (i = 0; i < 16; i++) { t = (uint32_t)r[i] - borrow; r[i] = (uint16_t)t; borrow = (t >> 16) & 1; }
    r[0] = (uint16_t)(r[0] - (uint16_t)borrow * 38u);
}

static void fe_mul(fe r, const fe a, const fe b) {
    uint16_t t[32];
    uint32_t c;
    uint8_t i, j;
    for (i = 0; i < 32; i++) t[i] = 0;
    for (i = 0; i < 16; i++) {
        uint16_t ai = a[i];
        c = 0;
        for (j = 0; j < 16; j++) {
            c += (uint32_t)ai * b[j];       /* 最大 (2^16-1)^2 に 2 つの 16 ビットを足しても 32 ビットに収まる */
            c += t[i + j];
            t[i + j] = (uint16_t)c;
            c >>= 16;
        }
        t[i + 16] = (uint16_t)c;
    }
    c = 0;
    for (i = 0; i < 16; i++) { c += t[i]; c += (uint32_t)t[i + 16] * 38u; r[i] = (uint16_t)c; c >>= 16; }
    fold(r, c);
}

static void fe_copy(fe r, const fe a) { uint8_t i; for (i = 0; i < 16; i++) r[i] = a[i]; }

static void fe_cswap(fe a, fe b, uint16_t bit) {
    uint16_t mask = (uint16_t)(0u - bit), x;
    uint8_t i;
    for (i = 0; i < 16; i++) { x = (uint16_t)((a[i] ^ b[i]) & mask); a[i] ^= x; b[i] ^= x; }
}

/* 指数 p-2 = 2^255-21 で逆数を求める(指数は公開の値) */
static void fe_invert(fe r, const fe a) {
    fe c;
    int16_t i;
    fe_copy(c, a);
    for (i = 253; i >= 0; i--) {
        fe_mul(c, c, c);
        if (i != 2 && i != 4) fe_mul(c, c, a);
    }
    fe_copy(r, c);
}

/* 2^255-19 未満の値にして、小さい順のバイト列にする */
static void fe_tobytes(uint8_t out[32], const fe a) {
    fe t, m;
    uint32_t c;
    uint16_t top, mask, x;
    uint8_t i, n;
    fe_copy(t, a);
    /* 上の 1 ビットを 19 倍して下へ戻す(2 回で 2^255 未満になる) */
    for (n = 0; n < 2; n++) {
        top = t[15] >> 15;
        t[15] &= 0x7fff;
        c = (uint32_t)top * 19u;
        for (i = 0; i < 16; i++) { c += t[i]; t[i] = (uint16_t)c; c >>= 16; }
    }
    /* t+19 が 2^255 に届けば t は p 以上なので、t-p = (t+19)-2^255 を使う */
    c = 19;
    for (i = 0; i < 16; i++) { c += t[i]; m[i] = (uint16_t)c; c >>= 16; }
    mask = (uint16_t)(0u - (uint16_t)(m[15] >> 15));
    m[15] &= 0x7fff;
    for (i = 0; i < 16; i++) { x = (uint16_t)((t[i] ^ m[i]) & mask); t[i] ^= x; }
    for (i = 0; i < 16; i++) { out[2 * i] = (uint8_t)t[i]; out[2 * i + 1] = (uint8_t)(t[i] >> 8); }
}

void oldp_x25519(uint8_t out[32], const uint8_t k[32], const uint8_t u[32]) {
    static const fe A24 = { 0xdb41, 1 };     /* 121665 */
    uint8_t e[32];
    fe x1, x2, z2, x3, z3, a, aa, b, bb, ee, c, d, da, cb;
    uint16_t swap = 0, kt;
    int16_t t;
    uint8_t i;
    for (i = 0; i < 32; i++) e[i] = k[i];
    e[0] &= 248; e[31] &= 127; e[31] |= 64;
    for (i = 0; i < 16; i++) x1[i] = (uint16_t)(u[2 * i] | ((uint16_t)u[2 * i + 1] << 8));
    x1[15] &= 0x7fff;
    for (i = 0; i < 16; i++) { x2[i] = 0; z2[i] = 0; z3[i] = 0; x3[i] = x1[i]; }
    x2[0] = 1; z3[0] = 1;
    for (t = 254; t >= 0; t--) {
        kt = (uint16_t)((e[t >> 3] >> (t & 7)) & 1);
        swap ^= kt;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = kt;
        fe_add(a, x2, z2);
        fe_mul(aa, a, a);
        fe_sub(b, x2, z2);
        fe_mul(bb, b, b);
        fe_sub(ee, aa, bb);
        fe_add(c, x3, z3);
        fe_sub(d, x3, z3);
        fe_mul(da, d, a);
        fe_mul(cb, c, b);
        fe_add(x3, da, cb);
        fe_mul(x3, x3, x3);
        fe_sub(z3, da, cb);
        fe_mul(z3, z3, z3);
        fe_mul(z3, z3, x1);
        fe_mul(x2, aa, bb);
        fe_mul(z2, A24, ee);
        fe_add(z2, z2, aa);
        fe_mul(z2, z2, ee);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);
    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_tobytes(out, x2);
    /* 秘密の値を消す */
    for (i = 0; i < 32; i++) *(volatile uint8_t *)&e[i] = 0;
    for (i = 0; i < 16; i++) {
        *(volatile uint16_t *)&x2[i] = 0; *(volatile uint16_t *)&z2[i] = 0;
        *(volatile uint16_t *)&x3[i] = 0; *(volatile uint16_t *)&z3[i] = 0;
    }
}

void oldp_x25519_base(uint8_t pub[32], const uint8_t k[32]) {
    uint8_t base[32];
    uint8_t i;
    base[0] = 9;
    for (i = 1; i < 32; i++) base[i] = 0;
    oldp_x25519(pub, k, base);
}
