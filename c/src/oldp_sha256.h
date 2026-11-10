/* SHA-256 と HMAC-SHA-256(FIPS 180-4・RFC 2104)。Noise のハッシュと HKDF、フィンガープリントに使う。 */
#ifndef OLDP_SHA256_H
#define OLDP_SHA256_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t h[8];
    uint8_t buf[64];
    uint32_t bits_lo, bits_hi;   /* 入れたビット数 */
    uint8_t used;                /* buf に入っているバイト数 */
} oldp_sha256_ctx;

void oldp_sha256_init(oldp_sha256_ctx *c);
void oldp_sha256_update(oldp_sha256_ctx *c, const uint8_t *data, size_t n);
void oldp_sha256_final(oldp_sha256_ctx *c, uint8_t out[32]);
void oldp_sha256(uint8_t out[32], const uint8_t *data, size_t n);

typedef struct { oldp_sha256_ctx inner; uint8_t okey[64]; } oldp_hmac_ctx;
void oldp_hmac_init(oldp_hmac_ctx *c, const uint8_t *key, size_t klen);
void oldp_hmac_update(oldp_hmac_ctx *c, const uint8_t *data, size_t n);
void oldp_hmac_final(oldp_hmac_ctx *c, uint8_t out[32]);

#endif
