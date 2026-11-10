#include "oldp_noise.h"
#include "oldp_sha256.h"
#include "oldp_x25519.h"
#include "monocypher.h"
#include <string.h>

/* 32 文字なので、h はこの名前そのもの(ハッシュしない) */
static const char PROTOCOL[] = "Noise_XX_25519_ChaChaPoly_SHA256";

/* ChaCha20-Poly1305 の nonce: 32 ビットの 0 と、64 ビットの小さい順のシーケンス番号 */
static void nonce_of(uint64_t n, uint8_t out[12]) {
    uint8_t i;
    memset(out, 0, 4);
    for (i = 0; i < 8; i++) out[4 + i] = (uint8_t)(n >> (8 * i));
}

int oldp_cipher_encrypt(oldp_cipher *c, const uint8_t *ad, size_t adlen, const uint8_t *pt, size_t len, uint8_t *out, size_t *out_len) {
    crypto_aead_ctx a;
    uint8_t nonce[12];
    if (!c->has_key) { memmove(out, pt, len); *out_len = len; return 0; }
    if (c->n == UINT64_MAX) return -1;
    nonce_of(c->n, nonce);
    crypto_aead_init_ietf(&a, c->k, nonce);
    crypto_aead_write(&a, out, out + len, ad, adlen, pt, len);
    crypto_wipe(&a, sizeof a);
    c->n++;
    *out_len = len + OLDP_TAGLEN;
    return 0;
}

int oldp_cipher_decrypt(oldp_cipher *c, const uint8_t *ad, size_t adlen, const uint8_t *ct, size_t len, uint8_t *out, size_t *out_len) {
    crypto_aead_ctx a;
    uint8_t nonce[12];
    int bad;
    if (!c->has_key) { memmove(out, ct, len); *out_len = len; return 0; }
    if (len < OLDP_TAGLEN || c->n == UINT64_MAX) return -1;
    nonce_of(c->n, nonce);
    crypto_aead_init_ietf(&a, c->k, nonce);
    bad = crypto_aead_read(&a, out, ct + len - OLDP_TAGLEN, ad, adlen, ct, len - OLDP_TAGLEN);
    crypto_wipe(&a, sizeof a);
    if (bad) return -1;
    c->n++;
    *out_len = len - OLDP_TAGLEN;
    return 0;
}

/* ---- SymmetricState ---- */
static void mix_hash(oldp_hs *hs, const uint8_t *data, size_t n) {
    oldp_sha256_ctx c;
    oldp_sha256_init(&c);
    oldp_sha256_update(&c, hs->h, 32);
    oldp_sha256_update(&c, data, n);
    oldp_sha256_final(&c, hs->h);
}

/* HKDF(ck, ikm) の 2 つの出力 */
static void hkdf2(const uint8_t ck[32], const uint8_t *ikm, size_t n, uint8_t o1[32], uint8_t o2[32]) {
    oldp_hmac_ctx m;
    uint8_t temp[32], b = 1;
    oldp_hmac_init(&m, ck, 32); oldp_hmac_update(&m, ikm, n); oldp_hmac_final(&m, temp);
    oldp_hmac_init(&m, temp, 32); oldp_hmac_update(&m, &b, 1); oldp_hmac_final(&m, o1);
    b = 2;
    oldp_hmac_init(&m, temp, 32); oldp_hmac_update(&m, o1, 32); oldp_hmac_update(&m, &b, 1); oldp_hmac_final(&m, o2);
    crypto_wipe(temp, sizeof temp);
}

static void mix_key(oldp_hs *hs, const uint8_t ikm[32]) {
    uint8_t ck[32], k[32];
    hkdf2(hs->ck, ikm, 32, ck, k);
    memcpy(hs->ck, ck, 32);
    memcpy(hs->cs.k, k, 32);
    hs->cs.has_key = 1;
    hs->cs.n = 0;
    crypto_wipe(k, sizeof k);
}

/* 結果がすべて 0(相手の公開鍵が位数の小さい点)なら -1 */
static int mix_dh(oldp_hs *hs, const uint8_t priv[32], const uint8_t pub[32]) {
    uint8_t shared[32], any = 0, i;
    oldp_x25519(shared, priv, pub);
    for (i = 0; i < 32; i++) any |= shared[i];
    mix_key(hs, shared);
    crypto_wipe(shared, sizeof shared);
    return any ? 0 : -1;
}

static int encrypt_and_hash(oldp_hs *hs, const uint8_t *pt, size_t len, uint8_t *out, size_t *out_len) {
    uint8_t h[32];
    memcpy(h, hs->h, 32);
    if (oldp_cipher_encrypt(&hs->cs, h, 32, pt, len, out, out_len)) return -1;
    mix_hash(hs, out, *out_len);
    return 0;
}

static int decrypt_and_hash(oldp_hs *hs, const uint8_t *ct, size_t len, uint8_t *out, size_t *out_len) {
    uint8_t h[32];
    memcpy(h, hs->h, 32);
    if (oldp_cipher_decrypt(&hs->cs, h, 32, ct, len, out, out_len)) return -1;
    mix_hash(hs, ct, len);
    return 0;
}

void oldp_public_key(uint8_t pub[32], const uint8_t priv[32]) { oldp_x25519_base(pub, priv); }

void oldp_hs_init(oldp_hs *hs, int initiator, const uint8_t s_priv[32], const uint8_t *prologue, size_t plen) {
    oldp_hs_init_pub(hs, initiator, s_priv, NULL, prologue, plen);
}

void oldp_hs_init_pub(oldp_hs *hs, int initiator, const uint8_t s_priv[32], const uint8_t *s_pub, const uint8_t *prologue, size_t plen) {
    memset(hs, 0, sizeof *hs);
    hs->initiator = (uint8_t)(initiator ? 1 : 0);
    memcpy(hs->s_priv, s_priv, 32);
    if (s_pub) memcpy(hs->s_pub, s_pub, 32); else oldp_public_key(hs->s_pub, s_priv);
    memcpy(hs->h, PROTOCOL, 32);
    memcpy(hs->ck, hs->h, 32);
    mix_hash(hs, prologue, plen);
}

void oldp_hs_set_ephemeral(oldp_hs *hs, const uint8_t e_priv[32]) {
    memcpy(hs->e_priv, e_priv, 32);
    hs->has_fixed_e = 1;
}

static void new_ephemeral(oldp_hs *hs, oldp_rng rng) {
    if (!hs->has_fixed_e) rng(hs->e_priv, 32);
    oldp_public_key(hs->e_pub, hs->e_priv);
}

static void split(oldp_hs *hs) {
    uint8_t k1[32], k2[32], zero = 0;
    hkdf2(hs->ck, &zero, 0, k1, k2);
    /* k1 は initiator から responder へ、k2 はその逆 */
    memcpy(hs->initiator ? hs->send.k : hs->recv.k, k1, 32);
    memcpy(hs->initiator ? hs->recv.k : hs->send.k, k2, 32);
    hs->send.has_key = hs->recv.has_key = 1;
    hs->send.n = hs->recv.n = 0;
    crypto_wipe(k1, sizeof k1);
    crypto_wipe(k2, sizeof k2);
    crypto_wipe(hs->e_priv, 32);
    crypto_wipe(&hs->cs, sizeof hs->cs);
}

int oldp_hs_write(oldp_hs *hs, const uint8_t *payload, size_t plen, uint8_t *out, size_t cap, size_t *out_len, oldp_rng rng) {
    size_t o = 0, n;
    int mine = hs->initiator ? (hs->step == 0 || hs->step == 2) : (hs->step == 1);
    if (!mine || hs->step > 2) return -1;
    if (cap < 32 + 48 + plen + OLDP_TAGLEN) return -1;
    if (hs->step == 0) {                          /* -> e */
        new_ephemeral(hs, rng);
        memcpy(out, hs->e_pub, 32); o = 32; mix_hash(hs, hs->e_pub, 32);
    } else if (hs->step == 1) {                   /* <- e, ee, s, es */
        new_ephemeral(hs, rng);
        memcpy(out, hs->e_pub, 32); o = 32; mix_hash(hs, hs->e_pub, 32);
        if (mix_dh(hs, hs->e_priv, hs->re)) return -1;
        if (encrypt_and_hash(hs, hs->s_pub, 32, out + o, &n)) return -1;
        o += n;
        if (mix_dh(hs, hs->s_priv, hs->re)) return -1;
    } else {                                      /* -> s, se */
        if (encrypt_and_hash(hs, hs->s_pub, 32, out + o, &n)) return -1;
        o += n;
        if (mix_dh(hs, hs->s_priv, hs->re)) return -1;
    }
    if (encrypt_and_hash(hs, payload, plen, out + o, &n)) return -1;
    o += n;
    *out_len = o;
    if (++hs->step == 3) split(hs);
    return 0;
}

int oldp_hs_read(oldp_hs *hs, const uint8_t *msg, size_t len, uint8_t *payload, size_t cap, size_t *plen) {
    size_t i = 0, n;
    uint8_t rs[32];
    int theirs = hs->initiator ? (hs->step == 1) : (hs->step == 0 || hs->step == 2);
    if (!theirs || hs->step > 2) return -1;
    if (hs->step == 0) {                          /* e */
        if (len < 32) return -1;
        memcpy(hs->re, msg, 32); i = 32; mix_hash(hs, hs->re, 32);
    } else if (hs->step == 1) {                   /* e, ee, s, es */
        if (len < 32 + 32 + OLDP_TAGLEN) return -1;
        memcpy(hs->re, msg, 32); i = 32; mix_hash(hs, hs->re, 32);
        if (mix_dh(hs, hs->e_priv, hs->re)) return -1;
        if (decrypt_and_hash(hs, msg + i, 32 + OLDP_TAGLEN, rs, &n)) return -1;
        memcpy(hs->rs, rs, 32); i += 32 + OLDP_TAGLEN;
        if (mix_dh(hs, hs->e_priv, hs->rs)) return -1;
    } else {                                      /* s, se */
        if (len < 32 + OLDP_TAGLEN) return -1;
        if (decrypt_and_hash(hs, msg, 32 + OLDP_TAGLEN, rs, &n)) return -1;
        memcpy(hs->rs, rs, 32); i = 32 + OLDP_TAGLEN;
        if (mix_dh(hs, hs->e_priv, hs->rs)) return -1;
    }
    if (len - i > cap + (hs->cs.has_key ? OLDP_TAGLEN : 0)) return -1;
    if (decrypt_and_hash(hs, msg + i, len - i, payload, &n)) return -1;
    *plen = n;
    if (++hs->step == 3) split(hs);
    return 0;
}

void oldp_fingerprint(const uint8_t pub[32], char out[20]) {
    static const char A[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    uint8_t d[32], i, bits = 0, k = 0;
    uint16_t v = 0;
    oldp_sha256(d, pub, 32);
    for (i = 0; i < 10; i++) {
        v = (uint16_t)((v << 8) | d[i]); bits += 8;
        while (bits >= 5) {
            if (k == 4 || k == 9 || k == 14) out[k++] = '-';
            out[k++] = A[(v >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    out[19] = 0;
}
