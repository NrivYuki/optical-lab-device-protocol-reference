/* NIST SP 800-90B の 6.3 節(独立で同じ分布とは限らない値)の最小エントロピーの見積もり。
 * tools/h8-rngtrace.ps1 で集めた、1 バイトずつの値のファイルを読む。
 *
 *   gcc -O2 -o rng-estimate tools/rng-estimate.c -lm
 *   ./rng-estimate tools/.h8-rngtrace.bin        値そのもの(TCNT0 の下位 8 ビット)
 *   ./rng-estimate -d tools/.h8-rngtrace.bin     1 つ前の値との差(下位 8 ビット)
 *   ./rng-estimate -r 0.378 tools/.h8-restart.bin  再起動のテスト(3.1.4。最初のエントロピー H_I を渡す)
 *
 * 8 ビットの値には、6.3.1 と 6.3.5-6.3.10 の 7 つを使う(6.3.2-6.3.4 は 2 値だけに使う)。
 * ビットの並びに直した値(3.1.3。1 バイトを上位のビットから 8 ビット)には、10 個全部を使う。
 * 最初のエントロピーは、min(8 ビットの値での見積もり, 8 × ビットでの見積もり)である。
 * 製造者の主張との比較は行わない。
 *
 * 規格の書き方に決まりがない次の点は、次のように決めた。
 *   - MultiMMC と LZ78Y で、最も多い値が 2 つ以上あるときは、大きい値を選ぶ。
 *   - MultiMMC の maxEntries は、文脈(x)の数に対する上限として扱う。
 *   - t-Tuple と LRS では、部分列を 2 つの 61 ビットの法の多項式ハッシュで比べる(偶然の一致は無視できる)。
 *   - 4096 より長い繰り返しがあるときは、LRS をその長さまでで計算する(そうなる並びのエントロピーは、ほかの見積もりでもほぼ 0 になる)。 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define Z 2.576

static void *xmalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) { fprintf(stderr, "メモリが足りません\n"); exit(2); }
    return p;
}

static double upper(double p, double n) {
    double u = p + Z * sqrt(p * (1 - p) / (n - 1));
    return u > 1 ? 1 : u;
}

/* ---- 6.3.1 最頻値 ---- */
static double mcv(const uint8_t *s, size_t L, int k) {
    size_t c[256] = {0}, m = 0;
    for (size_t i = 0; i < L; i++) c[s[i]]++;
    for (int j = 0; j < k; j++) if (c[j] > m) m = c[j];
    return -log2(upper((double)m / L, (double)L));
}

/* ---- 6.3.2 衝突(2 値だけ) ---- */
static double collision(const uint8_t *s, size_t L) {
    size_t i = 0, v = 0;
    double sum = 0, sq = 0;
    while (i + 1 < L) {
        int t;
        if (s[i] == s[i + 1]) t = 2;
        else if (i + 2 < L) t = 3;
        else break;
        sum += t; sq += (double)t * t; v++; i += (size_t)t;
    }
    double x = sum / v, sd = sqrt((sq - v * x * x) / (v - 1));
    double xp = x - Z * sd / sqrt((double)v);
    double p;
    if (xp >= 2.5) p = 0.5;
    else if (xp <= 2) p = 1;
    else p = 0.5 + sqrt(1.25 - 0.5 * xp);
    return -log2(p);
}

/* ---- 6.3.3 マルコフ(2 値だけ) ---- */
static double markov(const uint8_t *s, size_t L) {
    double c0 = 0, c[2][2] = {{0, 0}, {0, 0}};
    for (size_t i = 0; i < L; i++) if (!s[i]) c0++;
    for (size_t i = 0; i + 1 < L; i++) c[s[i]][s[i + 1]]++;
    double P0 = c0 / L, P1 = 1 - P0;
    double P00 = c[0][0] / (c[0][0] + c[0][1]), P01 = 1 - P00;
    double P10 = c[1][0] / (c[1][0] + c[1][1]), P11 = 1 - P10;
    /* 対数で比べる(0 の確率は -inf) */
    double lp[6] = {
        log2(P0) + 127 * log2(P00),
        log2(P0) + 64 * log2(P01) + 63 * log2(P10),
        log2(P0) + log2(P01) + 126 * log2(P11),
        log2(P1) + log2(P10) + 126 * log2(P00),
        log2(P1) + 64 * log2(P10) + 63 * log2(P01),
        log2(P1) + 127 * log2(P11),
    };
    double m = -INFINITY;
    for (int i = 0; i < 6; i++) if (lp[i] > m) m = lp[i];
    double h = -m / 128;
    return h > 1 ? 1 : h;
}

/* ---- 6.3.4 圧縮(2 値だけ) ---- */
static double comp_G(double z, const double *lg, size_t d, size_t Lp, size_t v) {
    /* G(z) = 1/v Σ_{t=d+1}^{L'} [ Σ_{u=1}^{t-1} log2(u) z^2 (1-z)^(u-1) + log2(t) z (1-z)^(t-1) ] */
    double q = 1 - z, inner = 0, pw = 1, g = 0;   /* inner = Σ_{u=1}^{t-1} log2(u) (1-z)^(u-1)、pw = (1-z)^(t-1) */
    for (size_t t = 1; t <= Lp; t++) {
        if (t > d) g += z * z * inner + lg[t] * z * pw;
        inner += lg[t] * pw;
        pw *= q;
    }
    return g / v;
}

static double compression(const uint8_t *s, size_t L) {
    const int b = 6;
    const size_t d = 1000;
    size_t nb = L / b, v = nb - d;
    uint8_t *x = xmalloc(nb);
    for (size_t i = 0; i < nb; i++) { int a = 0; for (int j = 0; j < b; j++) a = a * 2 + s[i * b + j]; x[i] = (uint8_t)a; }
    size_t dict[64] = {0};
    for (size_t i = 1; i <= d; i++) dict[x[i - 1]] = i;
    double sum = 0, sq = 0;
    for (size_t i = d + 1; i <= nb; i++) {
        size_t D = dict[x[i - 1]] ? i - dict[x[i - 1]] : i;
        dict[x[i - 1]] = i;
        double l = log2((double)D);
        sum += l; sq += l * l;
    }
    free(x);
    double X = sum / v, sd = 0.5907 * sqrt(sq / (v - 1) - X * X);
    double Xp = X - Z * sd / sqrt((double)v);
    double *lg = xmalloc((nb + 1) * sizeof *lg);
    for (size_t t = 1; t <= nb; t++) lg[t] = log2((double)t);
    /* G(p) + (2^b - 1) G(q) = X' を p ∈ [2^-b, 1] で解く。左辺は p が大きいほど小さい */
    double lo = 1.0 / 64, hi = 1, p;
    double f_lo = comp_G(lo, lg, d, nb, v) + 63 * comp_G((1 - lo) / 63, lg, d, nb, v);
    if (Xp >= f_lo) { free(lg); return 1; }   /* 解がない: 1 ビット */
    for (int it = 0; it < 40; it++) {
        p = (lo + hi) / 2;
        double f = comp_G(p, lg, d, nb, v) + 63 * comp_G((1 - p) / 63, lg, d, nb, v);
        if (f > Xp) lo = p; else hi = p;
    }
    free(lg);
    p = (lo + hi) / 2;
    return -log2(p) / b;
}

/* ---- 部分列を数える(6.3.5、6.3.6) ---- */
#define M61 ((1ULL << 61) - 1)
static uint64_t mulmod(uint64_t a, uint64_t b) {
    __uint128_t r = (__uint128_t)a * b;
    uint64_t x = (uint64_t)(r & M61) + (uint64_t)(r >> 61);
    return x >= M61 ? x - M61 : x;
}

typedef struct { uint64_t a, b; uint32_t pos; } key_t2;
static int key_cmp(const void *x, const void *y) {
    const key_t2 *p = x, *q = y;
    if (p->a != q->a) return p->a < q->a ? -1 : 1;
    if (p->b != q->b) return p->b < q->b ? -1 : 1;
    return 0;
}

typedef struct {
    size_t L;
    uint64_t *ha, *hb, *pa, *pb;   /* 前からの累積のハッシュと、底の累乗 */
} hasher;

static void hasher_init(hasher *h, const uint8_t *s, size_t L) {
    const uint64_t BA = 1000003, BB = 911382323;
    h->L = L;
    h->ha = xmalloc((L + 1) * 8); h->hb = xmalloc((L + 1) * 8);
    h->pa = xmalloc((L + 1) * 8); h->pb = xmalloc((L + 1) * 8);
    h->pa[0] = h->pb[0] = 1;
    for (size_t i = 0; i < L; i++) {
        h->ha[i + 1] = (mulmod(h->ha[i], BA) + s[i] + 1) % M61;
        h->hb[i + 1] = (mulmod(h->hb[i], BB) + s[i] + 1) % M61;
        h->pa[i + 1] = mulmod(h->pa[i], BA);
        h->pb[i + 1] = mulmod(h->pb[i], BB);
    }
}
static void hasher_free(hasher *h) { free(h->ha); free(h->hb); free(h->pa); free(h->pb); }
static uint64_t win(const uint64_t *H, const uint64_t *P, size_t i, size_t W) {
    return (H[i + W] + M61 - mulmod(H[i], P[W])) % M61;
}

/* 長さ 1 から、繰り返しのある長さが尽きるまで数える。Q[W] は最も多い部分列の数、S[W] は Σ C(数, 2)。
 * 長さ W+1 で繰り返す部分列は、長さ W でも繰り返すので、繰り返した位置だけを次の長さで調べる */
static size_t count_tuples(const uint8_t *s, size_t L, double **Qo, double **So) {
    hasher h;
    hasher_init(&h, s, L);
    size_t cap = 64, n = L;
    double *Q = xmalloc(cap * sizeof *Q), *S = xmalloc(cap * sizeof *S);
    uint32_t *cand = xmalloc(L * sizeof *cand);
    key_t2 *k = xmalloc(L * sizeof *k);
    for (size_t i = 0; i < L; i++) cand[i] = (uint32_t)i;
    size_t W = 0;
    while (n) {
        W++;
        if (W >= cap) { cap *= 2; Q = realloc(Q, cap * sizeof *Q); S = realloc(S, cap * sizeof *S); }
        size_t m = 0;
        for (size_t j = 0; j < n; j++) {
            size_t i = cand[j];
            if (i + W > L) continue;
            k[m].a = win(h.ha, h.pa, i, W); k[m].b = win(h.hb, h.pb, i, W); k[m].pos = (uint32_t)i; m++;
        }
        qsort(k, m, sizeof *k, key_cmp);
        double q = 1, sum = 0;
        size_t nn = 0;
        for (size_t a = 0; a < m;) {
            size_t b = a + 1;
            while (b < m && !key_cmp(&k[a], &k[b])) b++;
            size_t c = b - a;
            if (c >= 2) {
                if (c > q) q = (double)c;
                sum += (double)c * (c - 1) / 2;
                for (size_t j = a; j < b; j++) cand[nn++] = k[j].pos;
            }
            a = b;
        }
        Q[W] = q; S[W] = sum;
        n = nn;
        if (!n) break;
        if (W >= 4096) {   /* ほとんど周期的な並び。この数え方では終わらないので、ここで止める(LRS の値は 4096 までで出す) */
            fprintf(stderr, "長さ 4096 を超える繰り返しがあるので、LRS は長さ 4096 までで計算します\n");
            W++;
            break;
        }
    }
    free(cand); free(k); hasher_free(&h);
    *Qo = Q; *So = S;
    return W;   /* S[W] = 0 になった長さ(LRS の v はその 1 つ前) */
}

static void tuple_lrs(const uint8_t *s, size_t L, double *h_tuple, double *h_lrs, size_t *tt, size_t *vv) {
    double *Q, *S;
    size_t top = count_tuples(s, L, &Q, &S);
    size_t t = 0;
    for (size_t W = 1; W < top; W++) if (Q[W] >= 35) t = W;
    double pmax = 0;
    for (size_t i = 1; i <= t; i++) {
        double p = pow(Q[i] / (double)(L - i + 1), 1.0 / i);
        if (p > pmax) pmax = p;
    }
    *h_tuple = t ? -log2(upper(pmax, (double)L)) : NAN;
    size_t u = t + 1, v = top - 1;
    pmax = 0;
    for (size_t W = u; W <= v; W++) {
        double n = (double)(L - W + 1);
        double p = pow(S[W] / (n * (n - 1) / 2), 1.0 / W);
        if (p > pmax) pmax = p;
    }
    *h_lrs = u <= v ? -log2(upper(pmax, (double)L)) : NAN;
    *tt = t; *vv = v;
    free(Q); free(S);
}

/* ---- 予測による見積もり(6.3.7-6.3.10 の最後) ---- */
static double predict_h(size_t N, size_t C, size_t run, int k) {
    double pg = (double)C / N;
    double pgu = C == 0 ? 1 - pow(0.01, 1.0 / N) : upper(pg, (double)N);
    double r = (double)run + 1, lo = 0, hi = 1;
    /* 0.99 = (1 - p x) / ((r + 1 - r x) q) / x^(N+1) を p について解く。右辺は p が大きいほど小さい */
    for (int it = 0; it < 100; it++) {
        double p = (lo + hi) / 2, q = 1 - p, x = 1;
        for (int j = 0; j < 10; j++) x = 1 + q * pow(p, r) * pow(x, r + 1);
        double lf = log(1 - p * x) - log((r + 1 - r * x) * q) - (N + 1.0) * log(x);
        if (isnan(lf) || lf < log(0.99)) hi = p; else lo = p;
    }
    double pl = (lo + hi) / 2;
    double p = pgu > pl ? pgu : pl;
    if (p < 1.0 / k) p = 1.0 / k;
    return -log2(p);
}

typedef struct { size_t N, C, run, cur; } tally;
static void tally_add(tally *t, int ok) {
    t->N++;
    if (ok) { t->C++; if (++t->cur > t->run) t->run = t->cur; } else t->cur = 0;
}

/* ---- 6.3.7 MultiMCW ---- */
static double multi_mcw(const uint8_t *s, size_t L, int k) {
    const size_t w[4] = {63, 255, 1023, 4095};
    uint32_t cnt[4][256];
    size_t last[256];
    memset(cnt, 0, sizeof cnt);
    for (int j = 0; j < 256; j++) last[j] = 0;
    size_t score[4] = {0};
    int winner = 0;
    tally t = {0};
    for (size_t i = 0; i < L; i++) {   /* i は 0 から。s[i] を予測するとき、s[0..i-1] を見る */
        if (i >= w[0]) {
            int f[4];
            for (int j = 0; j < 4; j++) {
                f[j] = -1;
                if (i < w[j]) continue;
                uint32_t m = 0;
                for (int y = 0; y < k; y++) {
                    if (cnt[j][y] > m || (cnt[j][y] == m && m && last[y] > last[f[j]])) { m = cnt[j][y]; f[j] = y; }
                }
            }
            tally_add(&t, f[winner] == s[i]);
            for (int j = 0; j < 4; j++) if (f[j] == s[i]) { score[j]++; if (score[j] >= score[winner]) winner = j; }
        }
        for (int j = 0; j < 4; j++) { cnt[j][s[i]]++; if (i >= w[j]) cnt[j][s[i - w[j]]]--; }
        last[s[i]] = i;
    }
    return predict_h(t.N, t.C, t.run, k);
}

/* ---- 6.3.8 Lag ---- */
static double lag(const uint8_t *s, size_t L, int k) {
    const size_t D = 128;
    size_t score[128] = {0};
    size_t winner = 0;
    tally t = {0};
    for (size_t i = 1; i < L; i++) {
        int pred = i > winner ? s[i - winner - 1] : -1;
        tally_add(&t, pred == s[i]);
        for (size_t d = 0; d < D && d < i; d++) if (s[i - d - 1] == s[i]) { score[d]++; if (score[d] >= score[winner]) winner = d; }
    }
    return predict_h(t.N, t.C, t.run, k);
}

/* ---- 文脈ごとの数の表(6.3.9、6.3.10) ---- */
typedef struct { uint64_t a, b; int32_t head; uint8_t used; } slot;
typedef struct { uint32_t count; int32_t next; uint8_t y; } node;
typedef struct { slot *t; size_t mask, n; } table;
static node *pool; static size_t pool_n, pool_cap;

static void table_init(table *tb, size_t bits) { tb->mask = ((size_t)1 << bits) - 1; tb->t = xmalloc((tb->mask + 1) * sizeof *tb->t); tb->n = 0; }
static slot *table_find(table *tb, uint64_t a, uint64_t b, int add) {
    size_t i = (size_t)(a ^ (b * 0x9e3779b97f4a7c15ULL)) & tb->mask;
    while (tb->t[i].used) {
        if (tb->t[i].a == a && tb->t[i].b == b) return &tb->t[i];
        i = (i + 1) & tb->mask;
    }
    if (!add) return NULL;
    if (tb->n * 2 > tb->mask) { fprintf(stderr, "表があふれました\n"); exit(2); }
    tb->t[i].used = 1; tb->t[i].a = a; tb->t[i].b = b; tb->t[i].head = -1; tb->n++;
    return &tb->t[i];
}
static void slot_inc(slot *e, uint8_t y) {
    for (int32_t p = e->head; p >= 0; p = pool[p].next) if (pool[p].y == y) { pool[p].count++; return; }
    if (pool_n == pool_cap) { pool_cap = pool_cap ? pool_cap * 2 : 1 << 20; pool = realloc(pool, pool_cap * sizeof *pool); if (!pool) exit(2); }
    pool[pool_n].y = y; pool[pool_n].count = 1; pool[pool_n].next = e->head; e->head = (int32_t)pool_n++;
}
/* 最も多い y(同じ数なら大きい y)。数を *c に返す */
static int slot_max(const slot *e, uint32_t *c) {
    int best = -1; uint32_t m = 0;
    for (int32_t p = e->head; p >= 0; p = pool[p].next)
        if (pool[p].count > m || (pool[p].count == m && pool[p].y > best)) { m = pool[p].count; best = pool[p].y; }
    *c = m;
    return best;
}

/* ---- 6.3.9 MultiMMC ---- */
static double multi_mmc(const uint8_t *s, size_t L, int k, hasher *h) {
    const size_t D = 16, MAXE = 100000;
    table tb[16];
    for (size_t d = 0; d < D; d++) table_init(&tb[d], 18);
    size_t score[16] = {0}, winner = 0;
    tally t = {0};
    pool_n = 0;
    for (size_t i = 2; i < L; i++) {   /* s[i] を予測する */
        /* 学ぶ: 文脈 s[i-d-1 .. i-2](長さ d)の次が s[i-1] */
        for (size_t d = 1; d <= D; d++) {
            if (d + 1 > i) break;
            size_t st = i - 1 - d;
            uint64_t a = win(h->ha, h->pa, st, d), b = win(h->hb, h->pb, st, d);
            slot *e = table_find(&tb[d - 1], a, b, tb[d - 1].n < MAXE);
            if (e) slot_inc(e, s[i - 1]);
        }
        int f[16];
        for (size_t d = 1; d <= D; d++) {
            f[d - 1] = -1;
            if (d > i) continue;
            size_t st = i - d;
            slot *e = table_find(&tb[d - 1], win(h->ha, h->pa, st, d), win(h->hb, h->pb, st, d), 0);
            uint32_t c;
            if (e) f[d - 1] = slot_max(e, &c);
        }
        tally_add(&t, f[winner] == s[i]);
        for (size_t d = 0; d < D; d++) if (f[d] == s[i]) { score[d]++; if (score[d] >= score[winner]) winner = d; }
    }
    for (size_t d = 0; d < D; d++) free(tb[d].t);
    return predict_h(t.N, t.C, t.run, k);
}

/* ---- 6.3.10 LZ78Y ---- */
static double lz78y(const uint8_t *s, size_t L, int k, hasher *h) {
    const size_t B = 16, MAXD = 65536;
    table tb;
    table_init(&tb, 18);   /* 長さの違う文脈は、ハッシュに長さを混ぜて分ける */
    tally t = {0};
    pool_n = 0;
    for (size_t i = B + 1; i < L; i++) {   /* s[i] を予測する */
        for (size_t j = B; j >= 1; j--) {
            size_t st = i - 1 - j;
            uint64_t a = win(h->ha, h->pa, st, j) ^ ((uint64_t)j << 56), b = win(h->hb, h->pb, st, j);
            slot *e = table_find(&tb, a, b, tb.n < MAXD);
            if (e) slot_inc(e, s[i - 1]);
        }
        int pred = -1;
        uint32_t maxc = 0;
        for (size_t j = B; j >= 1; j--) {
            size_t st = i - j;
            uint64_t a = win(h->ha, h->pa, st, j) ^ ((uint64_t)j << 56), b = win(h->hb, h->pb, st, j);
            slot *e = table_find(&tb, a, b, 0);
            uint32_t c;
            if (!e) continue;
            int y = slot_max(e, &c);
            if (c > maxc) { maxc = c; pred = y; }
        }
        tally_add(&t, pred == s[i]);
    }
    free(tb.t);
    return predict_h(t.N, t.C, t.run, k);
}

static double run_all(const char *name, const uint8_t *s, size_t L, int k) {
    double m = INFINITY, v;
    size_t tt, vv;
    hasher h;
    printf("%s(%zu 個、%d 通り)\n", name, L, k);
#define SHOW(label, expr) do { v = (expr); printf("  %-12s %.6f\n", label, v); if (!isnan(v) && v < m) m = v; fflush(stdout); } while (0)
    SHOW("MCV", mcv(s, L, k));
    if (k == 2) {
        SHOW("Collision", collision(s, L));
        SHOW("Markov", markov(s, L));
        SHOW("Compression", compression(s, L));
    }
    double ht, hl;
    tuple_lrs(s, L, &ht, &hl, &tt, &vv);
    SHOW("t-Tuple", ht);
    SHOW("LRS", hl);
    printf("    (t = %zu、最も長い繰り返し = %zu)\n", tt, vv);
    SHOW("MultiMCW", multi_mcw(s, L, k));
    SHOW("Lag", lag(s, L, k));
    hasher_init(&h, s, L);
    SHOW("MultiMMC", multi_mmc(s, L, k, &h));
    SHOW("LZ78Y", lz78y(s, L, k, &h));
    hasher_free(&h);
    printf("  最小         %.6f\n", m);
    return m;
}

/* 二項分布 B(n, p) で、X >= c となる確率 */
static double binom_tail(int n, double p, int c) {
    double s = 0;
    for (int k = c; k <= n; k++) s += exp(lgamma(n + 1.0) - lgamma(k + 1.0) - lgamma(n - k + 1.0) + k * log(p) + (n - k) * log1p(-p));
    return s;
}

/* 再起動のテスト(SP 800-90B 3.1.4)。1000 回の起動で 1000 個ずつ集めた値(1 行が 1 回の起動)。
 * 値は、行ごとに 1 つ前との差(行の最初は起動したときの 0 との差、つまり値そのもの)にして扱う */
static int restart_test(const char *file, double hi) {
    const int R = 1000, C = 1000;
    FILE *f = fopen(file, "rb");
    if (!f) { perror(file); return 2; }
    uint8_t *m = xmalloc((size_t)R * C), *t = xmalloc((size_t)R * C);
    if (fread(m, 1, (size_t)R * C, f) != (size_t)R * C) { fprintf(stderr, "1000 × 1000 個に足りません\n"); return 2; }
    fclose(f);
    for (int r = 0; r < R; r++)
        for (int c = C - 1; c > 0; c--) m[r * C + c] = (uint8_t)(m[r * C + c] - m[r * C + c - 1]);
    /* 3.1.4.3 の確かめ: 各行・各列の最頻値の数が、二項分布で決まる上限を超えないこと */
    double p = pow(2, -hi);
    int cut = 0;
    while (binom_tail(C, p, cut) > 0.000005) cut++;
    int worst_r = 0, worst_c = 0;
    for (int r = 0; r < R; r++) {
        int cnt[256] = {0}, mx = 0;
        for (int c = 0; c < C; c++) if (++cnt[m[r * C + c]] > mx) mx = cnt[m[r * C + c]];
        if (mx > worst_r) worst_r = mx;
    }
    for (int c = 0; c < C; c++) {
        int cnt[256] = {0}, mx = 0;
        for (int r = 0; r < R; r++) if (++cnt[m[r * C + c]] > mx) mx = cnt[m[r * C + c]];
        if (mx > worst_c) worst_c = mx;
    }
    printf("最初のエントロピー H_I = %.6f。最頻値の数の上限 %d(有意水準 0.000005)\n", hi, cut);
    printf("最頻値の数の最大: 行 %d、列 %d → %s\n", worst_r, worst_c, worst_r <= cut && worst_c <= cut ? "通った" : "通らない");
    for (int r = 0; r < R; r++) for (int c = 0; c < C; c++) t[c * R + r] = m[r * C + c];
    double hr = run_all("行の並び(起動ごと)", m, (size_t)R * C, 256);
    double hc = run_all("列の並び(起動から同じ順番の値)", t, (size_t)R * C, 256);
    double h = hi < hr ? hi : hr;
    if (hc < h) h = hc;
    printf("再起動のテストの後のエントロピー min(H_I, H_r, H_c) = min(%.6f, %.6f, %.6f) = %.6f ビット/値\n", hi, hr, hc, h);
    free(m); free(t);
    return worst_r <= cut && worst_c <= cut ? 0 : 1;
}

int main(int argc, char **argv) {
    int diff = 0;
    if (argc > 3 && !strcmp(argv[1], "-r")) return restart_test(argv[3], atof(argv[2]));
    if (argc > 1 && !strcmp(argv[1], "-d")) { diff = 1; argc--; argv++; }
    if (argc < 2) { fprintf(stderr, "使い方: rng-estimate [-d] ファイル\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    size_t L = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *s = xmalloc(L);
    if (fread(s, 1, L, f) != L) { perror("fread"); return 2; }
    fclose(f);
    if (diff) {
        for (size_t i = L - 1; i > 0; i--) s[i] = (uint8_t)(s[i] - s[i - 1]);
        memmove(s, s + 1, --L);
    }
    double h8 = run_all(diff ? "差の下位 8 ビット" : "TCNT0 の下位 8 ビット", s, L, 256);
    uint8_t *bits = xmalloc(L * 8);
    for (size_t i = 0; i < L; i++) for (int j = 0; j < 8; j++) bits[i * 8 + j] = (uint8_t)((s[i] >> (7 - j)) & 1);
    double h1 = run_all("ビットの並び", bits, L * 8, 2);
    double hi = h8 < 8 * h1 ? h8 : 8 * h1;
    printf("最初のエントロピー H_I = min(%.6f, 8 × %.6f) = %.6f ビット/値\n", h8, h1, hi);
    free(bits); free(s);
    return 0;
}
