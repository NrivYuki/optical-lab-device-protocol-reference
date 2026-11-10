#include "oldp_job.h"
#include "oldp_labels.h"
#include <string.h>

#define MAX_LINE 80
#define MAX_DIGITS 9

void oldp_job_init(oldp_job *j, const char *const *accept, const char *ext) {
    memset(j, 0, sizeof *j);
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

/* 整数(符号と 9 桁まで)。*neg に符号。だめなら 0 */
static int read_digits(const char *s, size_t n, int32_t *out) {
    int32_t v = 0;
    size_t i = 0, sig = 0;
    if (!n) return 0;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = sig || s[i] != '0' ? v * 10 + (s[i] - '0') : 0;
        if (v) sig++;
        if (sig > MAX_DIGITS) return 0;
    }
    *out = v;
    return 1;
}

static int check_int(const oldp_field *f, const char *s, size_t n) {
    int neg = n && s[0] == '-';
    int32_t v;
    if (!read_digits(s + neg, n - (size_t)neg, &v)) return 0;
    if (neg) v = -v;
    if ((f->flags & OLDP_F_HAS_MIN) && v < f->min) return 0;
    if ((f->flags & OLDP_F_HAS_MAX) && v > f->max) return 0;
    return 1;
}

static int check_dec(const oldp_field *f, const char *s, size_t n) {
    size_t i = 0, ip, fp = 0, k;
    int32_t v = 0;
    uint8_t digits = 0, started = 0;
    if (n && (s[0] == '+' || s[0] == '-')) {
        if (s[0] == '-' && !(f->flags & OLDP_F_SIGNED)) return 0;
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
    (void)v;
    return digits <= MAX_DIGITS;
}

static int check_text(const oldp_field *f, const char *s, size_t n) {
    size_t i;
    if (n > (size_t)(f->max ? f->max : MAX_LINE)) return 0;
    if ((f->flags & OLDP_F_DIGITS) && n != (size_t)f->max) return 0;
    for (i = 0; i < n; i++) {
        if (s[i] < 0x20 || s[i] > 0x7e || s[i] == '=' || s[i] == ';') return 0;
        if ((f->flags & OLDP_F_DIGITS) && (s[i] < '0' || s[i] > '9')) return 0;
    }
    return 1;
}

static int check_value(const oldp_field *f, const char *s, size_t n) {
    switch (f->type) {
    case OLDP_T_INT:  return check_int(f, s, n);
    case OLDP_T_DEC:  return check_dec(f, s, n);
    case OLDP_T_ENUM: return in_list(f->vals, '|', s, n);
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
    uint16_t count = 0;
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
            if (!check_value(f, s + i, e - i)) return 0;
        }
        count++;
        if (e >= n) break;
        i = e + 1;
    }
    switch (l->kind) {
    case OLDP_K_SINGLE: return count == 1;
    case OLDP_K_CHIRAL: return count == 2;
    case OLDP_K_EITHER: return count == 1 || count == 2;
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

int oldp_job_line(oldp_job *j, const char *line, size_t len) {
    size_t eq = 0, vlen;
    const char *val;
    const oldp_label *l;
    size_t ns;
    uint8_t r;
    j->lines++;
    while (eq < len && line[eq] != '=') eq++;
    if (len > MAX_LINE || eq == 0 || eq >= len) { j->malformed = 1; return OLDP_LINE_MALFORMED; }
    val = line + eq + 1;
    vlen = len - eq - 1;

    for (r = 0; r < OLDP_RULE_COUNT; r++) if (same(line, eq, OLDP_RULES[r].require)) j->rule_seen |= (uint8_t)(1u << r);

    if (same(line, eq, "TRCFMT")) {
        const char *v;
        size_t vn;
        close_trace(j);
        j->in_trace = 1; j->traces++; j->got_r = j->got_a = 0; j->want = -1; j->mode_u = 0;
        if (nth_value(val, vlen, 1, &v, &vn)) { int32_t w; if (read_digits(v, vn, &w)) j->want = w; }
        if (nth_value(val, vlen, 2, &v, &vn) && same(v, vn, "U")) j->mode_u = 1;
    } else if (j->in_trace && same(line, eq, "R")) j->got_r += count_values(val, vlen);
    else if (j->in_trace && same(line, eq, "A")) j->got_a += count_values(val, vlen);

    if (same(line, eq, "JOB")) return OLDP_LINE_USED;

    ns = vendor_ns(line, eq);
    l = ns ? NULL : find_label(line, eq);
    if (ns ? !(j->ext && in_list(j->ext, ';', line, ns)) : !l) { add_label(j, j->ignored, sizeof j->ignored, line, eq); return OLDP_LINE_IGNORED; }
    if (!accepted(j, line, eq)) { add_label(j, j->ignored, sizeof j->ignored, line, eq); return OLDP_LINE_IGNORED; }
    if (l && !check_record(l, val, vlen)) { add_label(j, j->rejected, sizeof j->rejected, line, eq); return OLDP_LINE_REJECTED; }

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
    for (r = 0; r < OLDP_RULE_COUNT; r++) {
        if ((j->rule_hit & (1u << r)) && !(j->rule_seen & (1u << r)))
            add_label(j, j->rejected, sizeof j->rejected, OLDP_RULES[r].label, strlen(OLDP_RULES[r].label));
    }
}

int oldp_job_status(const oldp_job *j) {
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
    if (j->bad_trace) add_label(j, j->rejected, sizeof j->rejected, "TRCFMT", 6);
    if (cap < n + 6 + 10) return 0;
    memcpy(buf, "JOB=", 4); memcpy(buf + 4, name, n); memcpy(buf + 4 + n, "\r\n", 2);
    o = n + 6;
    memcpy(buf + o, "STATUS=", 7); o += 7;
    buf[o++] = (char)('0' + st); buf[o++] = '\r'; buf[o++] = '\n';
    if (j->ignored[0] && !(o = put_list(buf, cap, o, "IGNORED", j->ignored))) return 0;
    if (j->rejected[0] && !(o = put_list(buf, cap, o, "REJECTED", j->rejected))) return 0;
    return o;
}
