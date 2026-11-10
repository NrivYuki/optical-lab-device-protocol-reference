/* COBS と CRC-32(規格 4 章の UART のトランスポート)。 */
#ifndef OLDP_COBS_H
#define OLDP_COBS_H
#include <stdint.h>
#include <stddef.h>

/* 0x00 を含まない形に詰める。書いた長さを返す(out は n + n/254 + 1 バイト以上) */
size_t oldp_cobs_encode(const uint8_t *in, size_t n, uint8_t *out);
/* 戻す。書いた長さを返す。形が正しくない・入りきらないときは (size_t)-1 */
size_t oldp_cobs_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap);
/* CRC-32(ISO-HDLC) */
uint32_t oldp_crc32(const uint8_t *data, size_t n);

#endif
