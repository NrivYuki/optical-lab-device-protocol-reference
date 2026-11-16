#include "oldp_job.h"
#include "oldp_labels.h"
#include <string.h>

#define MAX_LINE 80
#define MAX_DIGITS 9

typedef char oldp_sides_fits[OLDP_LABEL_COUNT <= 2 * sizeof(((oldp_job *)0)->sides) ? 1 : -1];
typedef char oldp_seen_fits[OLDP_LABEL_COUNT <= 8 * sizeof(((oldp_job *)0)->seen) ? 1 : -1];

void oldp_job_init(oldp_job *j, const char *const *accept, const char *ext) {
    memset(j, 0, sizeof *j);
    j->do_eyes = 0xff;
    j->accept = accept;
    j->ext = ext;
}

static int same(const char *a, size_t n, const char *z) {
    size_t k = strlen(z);
    return n == k && !memcmp(a, z, n);
}

/* 区切り(| か ;)で並んだ list の中に a があるか */
static int in_list(const char *list, char sep, const char *a, size_t n) {
    while (list && *list) {
        const char *e = list;
        while (*e && *e != sep) e++;
        if ((size_t)(e - list) == n && !memcmp(list, a, n)) return 1;
        list = *e ? e + 1 : e;
    }
    return 0;
}

/* ';' で並んだ list に、まだ無ければ足す。入らなければ truncated を立てる */
static void add_label(oldp_job *j, char *list, size_t cap, const char *a, size_t n) {
    size_t have = strlen(list);
    if (in_list(list, ';', a, n)) return;
    if (have + n + 2 > cap) { j->truncated = 1; return; }
    if (have) list[have++] = ';';
    memcpy(list + have, a, n);
    list[have + n] = 0;
}

/* 数字の並びを読む(符号は呼ぶ側が除く)。先頭の 0 を除いて 9 桁まで。だめなら 0 */
static int read_digits(const char *s, size_t n, int32_t *out) {
    int32_t v = 0;
    size_t i = 0, sig = 0;
    if (!n) return 0;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        if (!sig && s[i] == '0') continue;          /* 先頭の 0 */
        if (++sig > MAX_DIGITS) return 0;           /* 掛ける前に確かめる(int32_t をあふれさせない) */
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return 1;
}

/* 符号(+ か -)の付いてよい数を読む。だめなら 0 */
static int read_signed(const char *s, size_t n, int32_t *out) {
    size_t sign = n && (s[0] == '-' || s[0] == '+');
    if (!read_digits(s + sign, n - sign, out)) return 0;
    if (sign && s[0] == '-') *out = -*out;
    return 1;
}

static int check_int(const oldp_field *f, const char *s, size_t n) {
    int neg = n && s[0] == '-';
    size_t sign = n && (s[0] == '-' || s[0] == '+');   /* 符号は + か -。省いてよい(規格 10 章) */
    int32_t v;
    if (!read_digits(s + sign, n - sign, &v)) return 0;
    if (neg && (f->flags & OLDP_F_HAS_MIN) && f->min >= 0) return 0;   /* 負の値を取らない項目の -(-0 も) */
    if (neg) v = -v;
    if ((f->flags & OLDP_F_HAS_MIN) && v < f->min) return 0;
    if ((f->flags & OLDP_F_HAS_MAX) && v > f->max) return 0;
    return 1;
}

static int check_dec(const oldp_field *f, const char *s, size_t n) {
    size_t i = 0, ip, fp = 0, k;
    int32_t v = 0;
    uint8_t digits = 0, started = 0, neg = 0;
    if (n && (s[0] == '+' || s[0] == '-')) {
        if (s[0] == '-' && !(f->flags & OLDP_F_SIGNED)) return 0;
        neg = s[0] == '-';
        i = 1;
    }
    ip = i;
    while (i < n && s[i] >= '0' && s[i] <= '9') i++;
    if (i == ip) return 0;
    if (i < n) {
        if (s[i] != '.') return 0;
        k = i + 1;
        while (k < n && s[k] >= '0' && s[k] <= '9') { k++; fp++; }
        if (k != n || fp == 0 || fp > f->scale) return 0;
    }
    /* 10^scale 倍した整数の桁数(先頭の 0 は数えない) */
    for (k = ip; k < i; k++) {
        if (s[k] != '0') started = 1;
        if (started) digits++;
    }
    for (k = 0; k < fp; k++) {
        if (s[i + 1 + k] != '0') started = 1;
        if (started) digits++;
    }
    if (started) digits = (uint8_t)(digits + f->scale - fp);
    if (digits > MAX_DIGITS) return 0;
    if (!(f->flags & (OLDP_F_HAS_MIN | OLDP_F_HAS_MAX))) return 1;
    /* 10^scale 倍した値(9 桁までなので int32_t に収まる)で、min・max と比べる */
    for (k = ip; k < i; k++) v = v * 10 + (s[k] - '0');
    for (k = 0; k < f->scale; k++) v = v * 10 + (k < fp ? s[i + 1 + k] - '0' : 0);
    if (neg) v = -v;
    if ((f->flags & OLDP_F_HAS_MIN) && v < f->min) return 0;
    if ((f->flags & OLDP_F_HAS_MAX) && v > f->max) return 0;
    return 1;
}

/* 2 桁の数(s[0..1])が lo-hi か */
static int two(const char *s, int lo, int hi) {
    int v = (s[0] - '0') * 10 + (s[1] - '0');
    return v >= lo && v <= hi;
}

static int check_text(const oldp_field *f, const char *s, size_t n) {
    size_t i;
    if (n > (size_t)(f->max ? f->max : MAX_LINE)) return 0;
    for (i = 0; i < n; i++) {
        if (s[i] < 0x20 || s[i] > 0x7e || s[i] == ';') return 0;   /* 0x20-0x7E のうち ; を除く(規格 10 章) */
        if ((f->flags & OLDP_F_TIME) && (s[i] < '0' || s[i] > '9')) return 0;
    }
    /* 時刻 YYYYMMDDhhmmss: 月・日・時・分・秒の範囲 */
    if (f->flags & OLDP_F_TIME)
        return n == 14 && two(s + 4, 1, 12) && two(s + 6, 1, 31) && two(s + 8, 0, 23) && two(s + 10, 0, 59) && two(s + 12, 0, 59);
    return 1;
}

/* 決まった値。値が数のものは数として比べる(ETYP=+1 は 1。規格 10 章) */
static int check_enum(const oldp_field *f, const char *s, size_t n) {
    const char *list = f->vals;
    int neg = n && s[0] == '-';
    size_t sign = n && (s[0] == '-' || s[0] == '+');
    int32_t v, w;
    if (in_list(list, '|', s, n)) return 1;
    if (!read_digits(s + sign, n - sign, &v)) return 0;
    if (neg) v = -v;
    while (list && *list) {
        const char *e = list;
        size_t kn, ks;
        while (*e && *e != '|') e++;
        kn = (size_t)(e - list);
        ks = kn && list[0] == '-';
        if (kn > ks && read_digits(list + ks, kn - ks, &w) && (ks ? -w : w) == v) return 1;
        list = *e ? e + 1 : e;
    }
    return 0;
}

static int check_value(const oldp_field *f, const char *s, size_t n) {
    switch (f->type) {
    case OLDP_T_INT:  return check_int(f, s, n);
    case OLDP_T_DEC:  return check_dec(f, s, n);
    case OLDP_T_ENUM: return check_enum(f, s, n);
    default:         return check_text(f, s, n);
    }
}

static const oldp_label *find_label(const char *s, size_t n) {
    uint8_t i;
    for (i = 0; i < OLDP_LABEL_COUNT; i++) if (same(s, n, OLDP_LABELS[i].name)) return &OLDP_LABELS[i];
    return NULL;
}

/* 値の並びを確かめる。s は '=' の次から行の終わりまで */
static int check_record(const oldp_label *l, const char *s, size_t n) {
    size_t i = 0;
    uint16_t count = 0, empty = 0;
    for (;;) {
        size_t e = i;
        const oldp_field *f;
        while (e < n && s[e] != ';') e++;
        if (l->kind == OLDP_K_FIELDS) {
            if (count >= l->count) return 0;
            f = &OLDP_FIELDS[l->first + count];
            if (e == i) { if (!(f->flags & OLDP_F_OPTIONAL)) return 0; }
            else if (!check_value(f, s + i, e - i)) return 0;
        } else {
            f = &OLDP_FIELDS[l->first];
            /* 右眼;左眼の値は、片眼のジョブでは片側を空にする(規格 10 章)。空は後で数える */
            if (e == i && (l->kind == OLDP_K_CHIRAL || l->kind == OLDP_K_EITHER)) empty++;
            else if (!check_value(f, s + i, e - i)) return 0;
        }
        count++;
        if (e >= n) break;
        i = e + 1;
    }
    switch (l->kind) {
    case OLDP_K_SINGLE: return count == 1;
    case OLDP_K_CHIRAL: return count == 2 && empty < 2;
    case OLDP_K_EITHER: return (count == 1 && !empty) || (count == 2 && empty < 2);
    case OLDP_K_FIELDS: {
        uint16_t k;
        for (k = count; k < l->count; k++) if (!(OLDP_FIELDS[l->first + k].flags & OLDP_F_OPTIONAL)) return 0;
        return 1;
    }
    default: return 1;
    }
}

/* 「名前空間:名前」の形か。名前空間の長さを返す(違えば 0) */
static size_t vendor_ns(const char *s, size_t n) {
    size_t i = 0, c;
    if (!n || s[0] < 'A' || s[0] > 'Z') return 0;
    while (i < n && s[i] != ':') {
        if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9'))) return 0;
        i++;
    }
    if (i < 2 || i > 8 || i >= n) return 0;
    c = n - i - 1;
    if (c < 1 || c > 16) return 0;
    for (c = i + 1; c < n; c++) if (!((s[c] >= 'A' && s[c] <= 'Z') || (s[c] >= '0' && s[c] <= '9') || s[c] == '_')) return 0;
    return i;
}

static int accepted(const oldp_job *j, const char *s, size_t n) {
    const char *const *a = j->accept;
    if (!a) return 1;
    for (; *a; a++) if (same(s, n, *a)) return 1;
    return 0;
}

/* n 番目の値(';' 区切り)の範囲。無ければ 0 */
static int nth_value(const char *s, size_t n, uint8_t want, const char **v, size_t *vn) {
    size_t i = 0;
    uint8_t k = 0;
    for (;;) {
        size_t e = i;
        while (e < n && s[e] != ';') e++;
        if (k == want) { *v = s + i; *vn = e - i; return 1; }
        if (e >= n) return 0;
        i = e + 1; k++;
    }
}

static uint16_t count_values(const char *s, size_t n) {
    uint16_t c = 1;
    size_t i;
    for (i = 0; i < n; i++) if (s[i] == ';') c++;
    return c;
}

static void close_trace(oldp_job *j) {
    if (!j->in_trace) return;
    if (j->got_r != j->want || (j->mode_u && j->got_a != j->want)) j->bad_trace = 1;
}

/* A の値が増える一方か(規格 10 章)。だめなら bad_trace を立てる */
static void check_angles(oldp_job *j, const char *s, size_t n) {
    size_t i = 0;
    for (;;) {
        size_t e = i;
        int32_t v;
        while (e < n && s[e] != ';') e++;
        if (read_signed(s + i, e - i, &v)) {
            if (j->have_a && v <= j->last_a) j->bad_trace = 1;   /* 前の値より大きくする */
            j->last_a = v;
            j->have_a = 1;
        }
        if (e >= n) break;
        i = e + 1;
    }
}

/* 行の形: ラベルは英大文字・数字・_・:、文字は ASCII の 0x20-0x7E(規格 8 章) */
static int line_ok(const char *line, size_t len, size_t eq) {
    size_t i;
    for (i = 0; i < len; i++) if (line[i] < 0x20 || line[i] > 0x7e) return 0;
    for (i = 0; i < eq; i++) {
        char c = line[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == ':')) return 0;
    }
    return 1;
}

int oldp_job_line(oldp_job *j, const char *line, size_t len) {
    size_t eq = 0, vlen;
    const char *val;
    const oldp_label *l;
    size_t ns;
    uint8_t r;
    j->lines++;
    j->bytes += (uint32_t)len + 2;
    while (eq < len && line[eq] != '=') eq++;
    if (len > MAX_LINE || eq == 0 || eq >= len || !line_ok(line, len, eq)) { j->malformed = 1; return OLDP_LINE_MALFORMED; }
    val = line + eq + 1;
    vlen = len - eq - 1;


    if (same(line, eq, "TRCFMT")) {
        const char *v;
        size_t vn;
        close_trace(j);
        j->in_trace = 1; j->traces++; j->got_r = j->got_a = 0; j->want = -1; j->mode_u = 0; j->have_a = 0;
        if (nth_value(val, vlen, 1, &v, &vn)) { int32_t w; if (read_signed(v, vn, &w)) j->want = w; }
        if (nth_value(val, vlen, 2, &v, &vn) && same(v, vn, "U")) j->mode_u = 1;
    } else if (j->in_trace && same(line, eq, "R")) {
        if (j->got_a) j->bad_trace = 1;                 /* R の行は A の行より前(規格 10 章) */
        j->got_r += count_values(val, vlen);
    } else if (j->in_trace && same(line, eq, "A")) {
        j->got_a += count_values(val, vlen);
        check_angles(j, val, vlen);
    }

    if (same(line, eq, "JOB")) {
        /* JOB は分ける対象にしないが、値を受けられなければ REJECTED=JOB */
        unsigned k;
        int ok;
        l = find_label(line, eq);
        ok = !l || check_record(l, val, vlen);
        if (l) {   /* JOB の 2 度目も受けられない */
            k = (unsigned)(l - OLDP_LABELS);
            if (j->seen[k / 8] & (1u << (k % 8))) ok = 0;
            j->seen[k / 8] |= (uint8_t)(1u << (k % 8));
        }
        if (!ok) { add_label(j, j->rejected, sizeof j->rejected, line, eq); return OLDP_LINE_REJECTED; }
        return OLDP_LINE_USED;
    }

    ns = vendor_ns(line, eq);
    l = ns ? NULL : find_label(line, eq);
    if (ns ? !(j->ext && in_list(j->ext, ';', line, ns)) : !l) { add_label(j, j->ignored, sizeof j->ignored, line, eq); return OLDP_LINE_IGNORED; }
    if (!accepted(j, line, eq)) { add_label(j, j->ignored, sizeof j->ignored, line, eq); return OLDP_LINE_IGNORED; }
    /* rules の require は、捨てていない行で満たす(規格 10 章) */
    for (r = 0; r < OLDP_RULE_COUNT; r++) if (same(line, eq, OLDP_RULES[r].require)) j->rule_seen |= (uint8_t)(1u << r);
    {
        int ok = !l || check_record(l, val, vlen);
        if (l && ok && (l->kind == OLDP_K_SINGLE || l->kind == OLDP_K_EITHER) && !same(line, eq, "TRCFMT")) {
            /* 値が 1 つの項目の 2 度目(TRCFMT は玉型ごとに数える) */
            unsigned k = (unsigned)(l - OLDP_LABELS);
            if (j->seen[k / 8] & (1u << (k % 8))) ok = 0;
            j->seen[k / 8] |= (uint8_t)(1u << (k % 8));
        }
        if (l && (l->kind == OLDP_K_CHIRAL || l->kind == OLDP_K_EITHER) && count_values(val, vlen) == 2) {
            /* 値のある側(1 = 右眼、2 = 左眼)を覚え、ジョブの眼に足す。食い違いは全部の行を読んでから確かめる */
            const char *v;
            size_t vn;
            unsigned k = (unsigned)(l - OLDP_LABELS), sh = (k % 2) * 4;
            uint8_t m = 0, old = (uint8_t)((j->sides[k / 2] >> sh) & 15);
            if (nth_value(val, vlen, 0, &v, &vn) && vn) m |= 1;
            if (nth_value(val, vlen, 1, &v, &vn) && vn) m |= 2;
            j->eyes |= m;
            if ((old & 4) && (old & 3) != m) old |= 8;
            old = (uint8_t)((old & 8) | 4 | m);
            j->sides[k / 2] = (uint8_t)((j->sides[k / 2] & ~(15u << sh)) | ((unsigned)old << sh));
        }
        if (same(line, eq, "TRCFMT")) {
            const char *v;
            size_t vn;
            uint8_t eye = 0;
            if (nth_value(val, vlen, 3, &v, &vn) && vn == 1) eye = v[0] == 'R' ? 1 : v[0] == 'L' ? 2 : 0;
            j->eyes |= eye;
            if (ok && eye && (j->trace_eyes & eye)) ok = 0;   /* 玉型は眼ごとに 1 つまで */
            j->trace_eyes |= eye;
            j->trace_ok = (uint8_t)ok;
        }
        else if ((same(line, eq, "R") || same(line, eq, "A")) && !j->trace_ok) ok = 0;   /* 受けられる TRCFMT に続かない */
        if (ok && same(line, eq, "A") && !j->mode_u) ok = 0;   /* 等角度の玉型には A を置かない */
        if (ok && same(line, eq, "DO") && j->do_eyes == 0xff)
            j->do_eyes = (uint8_t)(val[0] == 'R' ? 1 : val[0] == 'L' ? 2 : val[0] == 'B' ? 3 : 0);
        if (!ok) { add_label(j, j->rejected, sizeof j->rejected, line, eq); return OLDP_LINE_REJECTED; }
    }

    for (r = 0; r < OLDP_RULE_COUNT; r++) {
        const oldp_rule *u = &OLDP_RULES[r];
        const char *v;
        size_t vn;
        if (!same(line, eq, u->label)) continue;
        if (u->field >= 0) {
            if (nth_value(val, vlen, (uint8_t)u->field, &v, &vn) && in_list(u->in, '|', v, vn)) j->rule_hit |= (uint8_t)(1u << r);
        } else {
            uint8_t k;
            for (k = 0; nth_value(val, vlen, k, &v, &vn); k++) if (in_list(u->in, '|', v, vn)) j->rule_hit |= (uint8_t)(1u << r);
        }
    }
    return OLDP_LINE_USED;
}

void oldp_job_finish(oldp_job *j) {
    uint8_t r;
    close_trace(j);
    j->in_trace = 0;
    /* 点の数か行の順が合わない玉型は、REJECTED に TRCFMT を入れる(規格 10 章) */
    if (j->bad_trace) add_label(j, j->rejected, sizeof j->rejected, "TRCFMT", 6);
    /* 値のある側がジョブの眼と違う項目と、ジョブの眼に無い眼の玉型は受けない。ジョブの眼は DO があれば DO(規格 10 章) */
    {
        unsigned k;
        uint8_t eyes = j->do_eyes != 0xff ? j->do_eyes : j->eyes;
        for (k = 0; k < OLDP_LABEL_COUNT; k++) {
            uint8_t sd = (uint8_t)((j->sides[k / 2] >> ((k % 2) * 4)) & 15);
            if ((sd & 4) && ((sd & 8) || (sd & 3) != eyes))
                add_label(j, j->rejected, sizeof j->rejected, OLDP_LABELS[k].name, strlen(OLDP_LABELS[k].name));
        }
        if (j->trace_eyes & (uint8_t)~eyes) add_label(j, j->rejected, sizeof j->rejected, "TRCFMT", 6);
    }
    for (r = 0; r < OLDP_RULE_COUNT; r++) {
        if ((j->rule_hit & (1u << r)) && !(j->rule_seen & (1u << r)))
            add_label(j, j->rejected, sizeof j->rejected, OLDP_RULES[r].label, strlen(OLDP_RULES[r].label));
    }
}

int oldp_job_status(const oldp_job *j) {
    if (j->bytes > 262144UL) return 5;     /* 1 つのメッセージは 256 KiB まで(規格 7 章) */
    if (j->malformed) return 2;
    return j->rejected[0] || j->bad_trace ? 4 : 0;
}

/* LABEL=a;b;c の行を、80 文字を超えないように分けて足す */
static size_t put_list(char *buf, size_t cap, size_t o, const char *label, const char *list) {
    size_t lab = strlen(label), i = 0, n = strlen(list), used = 0;
    while (i < n) {
        size_t e = i;
        while (e < n && list[e] != ';') e++;
        if (used && used + 1 + (e - i) + 2 > MAX_LINE) { if (o + 2 > cap) return 0; buf[o++] = '\r'; buf[o++] = '\n'; used = 0; }
        if (o + lab + 1 + (e - i) + 3 > cap) return 0;
        if (!used) { memcpy(buf + o, label, lab); o += lab; buf[o++] = '='; used = lab + 1; }
        else { buf[o++] = ';'; used++; }
        memcpy(buf + o, list + i, e - i);
        o += e - i; used += e - i;
        i = e + 1;
    }
    if (used) { buf[o++] = '\r'; buf[o++] = '\n'; }
    return o;
}

size_t oldp_job_result(oldp_job *j, const char *name, char *buf, size_t cap) {
    size_t n = strlen(name), o;
    int st = oldp_job_status(j);
    if (cap < n + 6 + 10) return 0;
    memcpy(buf, "JOB=", 4); memcpy(buf + 4, name, n); memcpy(buf + 4 + n, "\r\n", 2);
    o = n + 6;
    memcpy(buf + o, "STATUS=", 7); o += 7;
    buf[o++] = (char)('0' + st); buf[o++] = '\r'; buf[o++] = '\n';
    if (j->ignored[0] && !(o = put_list(buf, cap, o, "IGNORED", j->ignored))) return 0;
    if (j->rejected[0] && !(o = put_list(buf, cap, o, "REJECTED", j->rejected))) return 0;
    return o;
}
