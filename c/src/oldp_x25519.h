/* X25519(RFC 7748)。16 ビットの CPU 向けの小さい実装 */
#ifndef OLDP_X25519_H
#define OLDP_X25519_H
#include <stdint.h>

void oldp_x25519(uint8_t out[32], const uint8_t k[32], const uint8_t u[32]);
void oldp_x25519_base(uint8_t pub[32], const uint8_t k[32]);

#endif
