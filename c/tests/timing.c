/* 秘密の値によって処理時間が変わらないかを、PC(x86-64)で統計で確かめる(dudect の方法。Reparaz ほか 2017)。
 *   ./timing [X25519 の回数] [ほかの回数] [null]
 * 計算ごとに、決まった 1 つの鍵(組 A)と、乱数の鍵(組 B)を乱数の順で混ぜた並びを先に全て用意し、それから順に計算して
 * 所要サイクル(rdtsc)を集める(測る直前に鍵を書くと、書いたばかりのメモリを読む待ちが測った時間に混ざるため)。
 * 大きい外れ値を何通りかの割合で切り落とし、Welch の t 検定で 2 つの組の平均を比べる。
 * |t| が 4.5 を超えれば、時間が鍵によって変わると判断する(dudect の基準)。
 * null を付けると、組 A も乱数の鍵にする(違いが無いはずの組で、測り方が偽の差を出さないかを確かめる)。
 * 計算: X25519(oldp_x25519)、ChaCha20-Poly1305 で 64 バイトを暗号化(oldp_cipher_encrypt)、HMAC-SHA-256(64 バイト)。 */
#define _DEFAULT_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x86intrin.h>
#include "oldp_noise.h"
#include "oldp_sha256.h"
#include "oldp_x25519.h"

static uint64_t rs = 0x2545f4914f6cdd1dULL;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static void rbytes(uint8_t *p, size_t n) { while (n--) *p++ = (uint8_t)rnd(); }

static volatile uint8_t sink;

static uint64_t run(int kind, const uint8_t *k) {
    uint8_t out[96], msg[64] = {0}, base[32] = {9};
    unsigned aux;
    uint64_t t0, t1;
    _mm_lfence();
    t0 = __rdtscp(&aux);
    if (kind == 0) oldp_x25519(out, k, base);
    else if (kind == 1) {
        oldp_cipher c;
        size_t n;
        memcpy(c.k, k, 32); c.has_key = 1; c.n = 0;
        oldp_cipher_encrypt(&c, msg, 2, msg, sizeof msg, out, &n);
    } else {
        oldp_hmac_ctx m;
        oldp_hmac_init(&m, k, 32); oldp_hmac_update(&m, msg, sizeof msg); oldp_hmac_final(&m, out);
    }
    t1 = __rdtscp(&aux);
    _mm_lfence();
    sink ^= out[0];
    return t1 - t0;
}

static int cmp(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv) {
    static const char *const NAME[] = { "X25519", "ChaCha20-Poly1305", "HMAC-SHA-256" };
    long counts[3] = { argc > 1 ? atol(argv[1]) : 20000, 0, 0 };
    int null = argc > 3 && !strcmp(argv[3], "null"), worst_kind = 0;
    double worst = 0;
    counts[1] = counts[2] = argc > 2 ? atol(argv[2]) : 200000;
    for (int kind = 0; kind < 3; kind++) {
        long n = counts[kind], na = 0, nb = 0;
        uint8_t fixed[32];
        uint8_t *keys = malloc((size_t)64 * 2 * n), *cls = malloc((size_t)n * 2);
        uint64_t *t = malloc(sizeof *t * (size_t)n * 2), *sorted = malloc(sizeof *t * (size_t)n * 2);
        rbytes(fixed, 32);
        for (long i = 0; i < 2 * n; i++) {
            uint8_t *k = keys + 64 * i;
            cls[i] = (uint8_t)(rnd() & 1);
            if (cls[i] || null) rbytes(k, 32); else memcpy(k, fixed, 32);
            if (cls[i]) nb++; else na++;
        }
        for (long i = 0; i < 1000 && i < 2 * n; i++) run(kind, keys + 64 * i);   /* 慣らし */
        for (long i = 0; i < 2 * n; i++) t[i] = run(kind, keys + 64 * i);
        memcpy(sorted, t, sizeof *t * (size_t)n * 2);
        qsort(sorted, (size_t)n * 2, sizeof *t, cmp);
        double maxt = 0;
        printf("%s(組 A %ld 回、組 B %ld 回、中央値 %llu サイクル)\n", NAME[kind], na, nb, (unsigned long long)sorted[n]);
        /* 切り落とす位置ごとに t を求める(dudect と同じく、外れ値の影響を見る) */
        static const double CROP[] = { 1.0, 0.99, 0.95, 0.9, 0.75, 0.5 };
        for (int c = 0; c < 6; c++) {
            uint64_t lim = sorted[(size_t)((double)(2 * n - 1) * CROP[c])];
            double s[2] = {0, 0}, q[2] = {0, 0}, m[2] = {0, 0};
            for (long i = 0; i < 2 * n; i++) {
                if (t[i] > lim) continue;
                double x = (double)t[i];
                s[cls[i]]++;
                double d = x - m[cls[i]];
                m[cls[i]] += d / s[cls[i]];
                q[cls[i]] += d * (x - m[cls[i]]);
            }
            double tv = (m[0] - m[1]) / sqrt(q[0] / (s[0] - 1) / s[0] + q[1] / (s[1] - 1) / s[1]);
            printf("  外れ値を切る位置 %3.0f%%: 平均 A %.2f / B %.2f、t = %+.2f\n", CROP[c] * 100, m[0], m[1], tv);
            if (fabs(tv) > maxt) maxt = fabs(tv);
        }
        printf("  → 最大の |t| = %.2f(%s)\n", maxt, maxt > 4.5 ? "鍵によって時間が変わる" : "違いは見つからない");
        if (maxt > worst) { worst = maxt; worst_kind = kind; }
        free(keys); free(cls); free(t); free(sorted);
    }
    printf("最も大きい |t| は %s の %.2f\n", NAME[worst_kind], worst);
    return worst > 4.5;
}
