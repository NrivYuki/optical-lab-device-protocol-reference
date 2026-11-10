/* H8 の組み立てには標準ライブラリがないので、OLDP の実装と Monocypher が使う分だけを用意する。 */
#ifndef OLDP_H8_STRING_H
#define OLDP_H8_STRING_H
#include <stddef.h>
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
#endif
