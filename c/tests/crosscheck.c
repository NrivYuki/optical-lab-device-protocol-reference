/* 機器の側の暗号の部品を、別の実装(tools/crosscheck.py が使う OpenSSL)と比べるための口。
 * 標準入力から 1 行に 1 つの指示を読み、結果を 1 行ずつ書く(値は 16 進)。
 *   x25519 <k> <u>                  → <out>
 *   sha256 <data>                   → <digest>
 *   hmac <key> <data>               → <mac>
 *   enc <k> <n(10 進)> <ad> <pt>    → <ct+tag> か fail
 *   dec <k> <n(10 進)> <ad> <ct>    → <pt> か fail
 * 空の値は "-" と書く。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "oldp_noise.h"
#include "oldp_sha256.h"
#include "oldp_x25519.h"

#define MAXB 70000
static uint8_t a[MAXB], b[MAXB], c[MAXB], d[MAXB + 16];

static size_t unhex(const char *s, uint8_t *out) {
    size_t n = 0;
    if (!strcmp(s, "-")) return 0;
    while (s[0] && s[1]) { unsigned v; sscanf(s, "%2x", &v); out[n++] = (uint8_t)v; s += 2; }
    return n;
}
static void hex(const uint8_t *p, size_t n) {
    if (!n) { fputs("-", stdout); }
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
    putchar('\n');
}

static char line[4 * MAXB + 64];

int main(void) {
    while (fgets(line, sizeof line, stdin)) {
        char *t[6] = {0};
        int n = 0;
        for (char *p = strtok(line, " \r\n"); p && n < 6; p = strtok(NULL, " \r\n")) t[n++] = p;
        if (!n) continue;
        if (!strcmp(t[0], "x25519")) {
            unhex(t[1], a); unhex(t[2], b);
            oldp_x25519(c, a, b);
            hex(c, 32);
        } else if (!strcmp(t[0], "sha256")) {
            size_t la = unhex(t[1], a);
            oldp_sha256(c, a, la);
            hex(c, 32);
        } else if (!strcmp(t[0], "hmac")) {
            size_t lk = unhex(t[1], a), ld = unhex(t[2], b);
            oldp_hmac_ctx m;
            oldp_hmac_init(&m, a, lk); oldp_hmac_update(&m, b, ld); oldp_hmac_final(&m, c);
            hex(c, 32);
        } else if (!strcmp(t[0], "enc") || !strcmp(t[0], "dec")) {
            oldp_cipher ci;
            memset(&ci, 0, sizeof ci);
            unhex(t[1], ci.k);
            ci.has_key = 1;
            ci.n = strtoull(t[2], NULL, 10);
            size_t lad = unhex(t[3], a), lx = unhex(t[4], b), lo = 0;
            int r = t[0][0] == 'e' ? oldp_cipher_encrypt(&ci, a, lad, b, lx, d, &lo)
                                   : oldp_cipher_decrypt(&ci, a, lad, b, lx, d, &lo);
            if (r) puts("fail"); else hex(d, lo);
        } else {
            puts("?");
        }
        fflush(stdout);
    }
    return 0;
}
