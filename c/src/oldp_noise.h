/* Noise Protocol Framework(第 34 版)の Noise_XX_25519_ChaChaPoly_SHA256(規格 6 章)。
 * X25519 と ChaCha20-Poly1305 は Monocypher、SHA-256 と HMAC は oldp_sha256 を使う。動的なメモリは使わない。 */
#ifndef OLDP_NOISE_H
#define OLDP_NOISE_H
#include <stdint.h>
#include <stddef.h>

#define OLDP_KEYLEN 32
#define OLDP_TAGLEN 16

typedef struct {
    uint8_t k[32];
    uint8_t has_key;
    uint64_t n;
} oldp_cipher;

/* 暗号化して out に書く(鍵が無ければそのまま写す)。out_len に長さ。0 か -1 */
int oldp_cipher_encrypt(oldp_cipher *c, const uint8_t *ad, size_t adlen, const uint8_t *pt, size_t len, uint8_t *out, size_t *out_len);
/* 解いて out に書く。改ざん・鍵の違いなら -1 */
int oldp_cipher_decrypt(oldp_cipher *c, const uint8_t *ad, size_t adlen, const uint8_t *ct, size_t len, uint8_t *out, size_t *out_len);

typedef void (*oldp_rng)(uint8_t *buf, size_t n);

typedef struct {
    uint8_t initiator, step, has_fixed_e;
    uint8_t s_priv[32], s_pub[32], e_priv[32], e_pub[32], rs[32], re[32];
    uint8_t h[32], ck[32];
    oldp_cipher cs;
    oldp_cipher send, recv;   /* ハンドシェイクを終えたあとの送受信 */
} oldp_hs;

/* initiator は機器(1・3 番目のメッセージを書く) */
void oldp_hs_init(oldp_hs *hs, int initiator, const uint8_t s_priv[32], const uint8_t *prologue, size_t plen);
/* 公開鍵が分かっているとき(s_pub が NULL なら計算する)。X25519 の遅いマイコンで 1 回分を省く */
void oldp_hs_init_pub(oldp_hs *hs, int initiator, const uint8_t s_priv[32], const uint8_t *s_pub, const uint8_t *prologue, size_t plen);
/* テストベクタのためだけに、一時鍵を決めて渡す(ふだんは rng で作る) */
void oldp_hs_set_ephemeral(oldp_hs *hs, const uint8_t e_priv[32]);
/* 自分の番のメッセージを書く。-1 は順番違い・入りきらない */
int oldp_hs_write(oldp_hs *hs, const uint8_t *payload, size_t plen, uint8_t *out, size_t cap, size_t *out_len, oldp_rng rng);
/* 相手のメッセージを読み、中の payload を書く。-1 は順番違い・短すぎる・解けない */
int oldp_hs_read(oldp_hs *hs, const uint8_t *msg, size_t len, uint8_t *payload, size_t cap, size_t *plen);
/* ハンドシェイクを終えたか */
#define oldp_hs_done(hs) ((hs)->step == 3)

/* 公開鍵のフィンガープリント("XXXX-XXXX-XXXX-XXXX"。終わりの 0 を含めて 20 バイト) */
void oldp_fingerprint(const uint8_t pub[32], char out[20]);
/* 秘密鍵から公開鍵 */
void oldp_public_key(uint8_t pub[32], const uint8_t priv[32]);

#endif
