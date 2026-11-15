/* 機器の側の実装を、乱数で作った入力で叩く(AddressSanitizer と UndefinedBehaviorSanitizer を付けて組み立てる。c/Makefile の fuzz)。
 *   ./fuzz_device [セッションの数] [乱数の種]
 * 1 つのスレッドの中で、ホストの代わりをこのファイルが行う。機器が書いたフレームに、その場で返事を作って読み取りの列に積む。
 * セッションごとに、次のどれかを行う(TCP と UART を交互に使う)。
 *   - ハンドシェイクの前: 壊したフレーム・知らない種類・失敗のフレーム・壊した 2 つ目のメッセージ・おかしなケイパビリティを返す
 *   - ハンドシェイクの後(ペアリングしたホストが壊れたものを送る): 形の崩れた応答・続きのフラグの付いた応答・知らない操作・
 *     リクエスト ID の違う応答・長すぎる行・大きな数字・ホストからの要求を、正しく暗号化して返す
 *   - 線の上のバイト列そのもの(COBS・CRC・長さ)を乱数で壊す
 * 機器は、TIME・PING・PUT・GET(受けた行は oldp_job_line で確かめる)・RESULT・待ち受けを順に行う。
 * 確かめるのは、メモリの誤り・未定義の動作が起きないこと(Sanitizer が止める)と、受け入れの通知の前に操作を始めないこと。
 * あわせて、oldp_cobs_decode と oldp_job_line を、乱数のバイト列で直接叩く。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "oldp_device.h"
#include "oldp_job.h"
#include "oldp_cobs.h"

/* ---- 乱数(再現できるように種から作る) ---- */
static uint64_t rs = 88172645463325252ULL;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 32); }
static uint32_t below(uint32_t n) { return n ? rnd() % n : 0; }
static void rbytes(uint8_t *p, size_t n) { while (n--) *p++ = (uint8_t)rnd(); }
static void rng_fn(uint8_t *buf, size_t n) { rbytes(buf, n); }

/* ---- 機器へ渡すバイトの列と、機器が書いたバイト ---- */
#define QMAX (1 << 20)
static uint8_t inq[QMAX]; static size_t in_head, in_tail;
static uint8_t outb[1 << 17]; static size_t out_len;
static int transport;
static uint8_t host_seq;
static int mode;                 /* 0: ハンドシェイクの前を壊す、1: ハンドシェイクの後を壊す、2: 線の上のバイトを壊す */
static int accepted_sent;        /* 受け入れの通知を送ったか */
static int op_before_accept;     /* 受け入れの通知の前に、機器が操作を送った */

static void push(const uint8_t *p, size_t n) {
    if (in_tail + n > QMAX) return;
    if (mode == 2 && n && below(4) == 0) {   /* 線の上で壊す: 1 バイト書き換え・落とす・足す */
        uint8_t t[70000];
        size_t m = n < sizeof t ? n : sizeof t;
        memcpy(t, p, m);
        switch (below(3)) {
        case 0: t[below((uint32_t)m)] ^= (uint8_t)(1 << below(8)); break;
        case 1: if (m > 1) m = below((uint32_t)m); break;
        default: if (m < sizeof t) t[m++] = (uint8_t)rnd(); break;
        }
        memcpy(inq + in_tail, t, m); in_tail += m;
        return;
    }
    memcpy(inq + in_tail, p, n); in_tail += n;
}

/* ホストからのフレームを、トランスポートの形にして積む */
static void host_send(const uint8_t *f, size_t n) {
    if (transport == OLDP_TCP) {
        uint8_t h[2] = { (uint8_t)(n >> 8), (uint8_t)n };
        uint8_t t[70002];
        memcpy(t, h, 2); memcpy(t + 2, f, n);
        push(t, n + 2);
    } else {
        static uint8_t u[70010], c[71000];
        uint32_t crc;
        size_t m;
        u[0] = 0; u[1] = host_seq++;
        memcpy(u + 2, f, n);
        crc = oldp_crc32(u, n + 2);
        u[n + 2] = (uint8_t)(crc >> 24); u[n + 3] = (uint8_t)(crc >> 16); u[n + 4] = (uint8_t)(crc >> 8); u[n + 5] = (uint8_t)crc;
        m = oldp_cobs_encode(u, n + 6, c);
        c[m++] = 0;
        push(c, m);
    }
}
static void host_ack(uint8_t seq) {
    uint8_t u[6], c[16];
    uint32_t crc;
    size_t m;
    u[0] = 1; u[1] = seq;
    crc = oldp_crc32(u, 2);
    u[2] = (uint8_t)(crc >> 24); u[3] = (uint8_t)(crc >> 16); u[4] = (uint8_t)(crc >> 8); u[5] = (uint8_t)crc;
    m = oldp_cobs_encode(u, 6, c);
    c[m++] = 0;
    push(c, m);
}

/* ---- ホストの代わり ---- */
static oldp_hs host;
static uint8_t host_priv[32];
static const char PROLOGUE[] = "optical-lab-device-protocol 1";

static void host_data(uint8_t op, uint16_t id, uint8_t flags, const char *body, size_t len) {
    uint8_t plain[66000], f[66100];
    size_t ct;
    if (len > 65000) len = 65000;
    plain[0] = op; plain[1] = (uint8_t)(id >> 8); plain[2] = (uint8_t)id; plain[3] = flags;
    memcpy(plain + 4, body, len);
    f[0] = 1; f[1] = OLDP_DATA;
    if (oldp_cipher_encrypt(&host.send, f, 2, plain, len + 4, f + 2, &ct)) return;
    host_send(f, ct + 2);
}

/* 壊れた行を作る */
static size_t junk_lines(char *b, size_t cap) {
    static const char *const L[] = { "JOB=", "STATUS=", "TRCFMT=", "R=", "A=", "DBL=", "HBOX=", "ETYP=", "TIME=", "LABELS=", "ACME.X=", "=", "", "NOEQ", "R=;;;;", "TRCFMT=1;", "TRCFMT=1;9999999999;E;R;F", "STATUS=", "IGNORED=", "REJECTED=" };
    size_t n = 0, k = 1 + below(30);
    while (k-- && n + 300 < cap) {
        const char *l = L[below(sizeof L / sizeof L[0])];
        size_t m = strlen(l), j, extra = below(5) == 0 ? below(200) : below(12);
        memcpy(b + n, l, m); n += m;
        for (j = 0; j < extra; j++) {
            uint32_t r = below(10);
            b[n++] = r < 6 ? (char)('0' + below(10)) : r < 8 ? ';' : r < 9 ? (char)(rnd() & 0xff) : '-';
        }
        if (below(8)) { b[n++] = '\r'; b[n++] = '\n'; } else if (below(2)) b[n++] = '\n';
    }
    return n;
}

static void host_reply_to(const uint8_t *msg, size_t n) {
    char b[66000];
    size_t bl;
    uint8_t op = msg[0];
    uint16_t id = (uint16_t)((msg[1] << 8) | msg[2]);
    (void)n;
    if (mode != 1) {   /* 正しい応答 */
        if (op == OLDP_GET) {
            bl = (size_t)snprintf(b, sizeof b, "JOB=X\r\nDBL=18.00\r\nTRCFMT=1;4;E;R;F\r\nR=2400;2401;2402;2403\r\n");
            host_data(OLDP_DATA_OP, id, OLDP_RESPONSE, b, bl);
        } else if (op == OLDP_TIME) {
            host_data(OLDP_TIME, id, OLDP_RESPONSE, "TIME=20261114000000\r\n", 21);
        } else if (op == OLDP_PING) {
            host_data(OLDP_PING, id, OLDP_RESPONSE, "", 0);
        } else host_data(OLDP_RESULT, id, OLDP_RESPONSE, "STATUS=0\r\n", 10);
        return;
    }
    /* 壊した応答 */
    {
        uint32_t k = 1 + below(4);
        while (k--) {
            uint32_t w = below(10);
            uint8_t rop = below(3) ? (op == OLDP_GET ? OLDP_DATA_OP : op == OLDP_TIME || op == OLDP_PING ? op : OLDP_RESULT) : (uint8_t)rnd();
            uint16_t rid = below(4) ? id : (uint16_t)rnd();
            uint8_t fl = (uint8_t)(below(3) ? OLDP_RESPONSE : rnd());
            if (w == 0) { bl = below(2000); rbytes((uint8_t *)b, bl); }
            else if (w == 1) { bl = (size_t)snprintf(b, sizeof b, "STATUS=%u%u%u%u\r\n", rnd(), rnd(), rnd(), rnd()); }
            else if (w == 2) { bl = below(60000); memset(b, 'A', bl); }
            else if (w == 3) { /* ホストからの要求を挟む */
                bl = junk_lines(b, sizeof b);
                host_data((uint8_t)(below(2) ? OLDP_AVAIL : rnd()), (uint16_t)rnd(), (uint8_t)(rnd() & 1), b, bl);
                continue;
            } else bl = junk_lines(b, sizeof b);
            host_data(rop, rid, fl, b, bl);
        }
        /* 最後に、終わりのフラグの付いた応答を返すことがある(返さなければ機器は時間切れで終える) */
        if (below(2)) host_data(op == OLDP_GET ? OLDP_DATA_OP : OLDP_RESULT, id, OLDP_RESPONSE, "STATUS=0\r\n", 10);
    }
}

static void host_frame(const uint8_t *f, size_t n) {
    uint8_t buf[70000];
    size_t m;
    if (n < 2) return;
    if (f[1] == OLDP_HS1) {
        char caps[600];
        size_t cl = (size_t)snprintf(caps, sizeof caps, "VER=1.0\r\nDEV=HST\r\nVEN=T\r\nMODEL=H\r\nMAXFRAME=%u\r\n", below(3) ? 512 : rnd());
        oldp_hs_init(&host, 0, host_priv, (const uint8_t *)PROLOGUE, sizeof PROLOGUE - 1);
        if (oldp_hs_read(&host, f + 2, n - 2, buf, sizeof buf, &m)) return;
        if (mode == 0 && below(3) == 0) { cl = junk_lines(caps, sizeof caps - 300); }
        buf[0] = 1; buf[1] = OLDP_HS2;
        if (oldp_hs_write(&host, (const uint8_t *)caps, cl, buf + 2, sizeof buf - 2, &m, rng_fn)) return;
        m += 2;
        if (mode == 0) {
            switch (below(8)) {
            case 0: buf[2 + below((uint32_t)m - 2)] ^= (uint8_t)(1 << below(8)); break;   /* 2 つ目のメッセージを壊す */
            case 1: m = below((uint32_t)m); break;                                            /* 短くする */
            case 2: buf[1] = (uint8_t)rnd(); break;                                           /* 知らない種類 */
            case 3: buf[0] = (uint8_t)rnd(); break;                                           /* 違うバージョン */
            case 4: { uint8_t fl[64]; size_t k = below(60); fl[0] = 1; fl[1] = OLDP_FAIL; rbytes(fl + 2, k); host_send(fl, k + 2); return; }
            default: break;
            }
        }
        host_send(buf, m);
    } else if (f[1] == OLDP_HS3) {
        if (oldp_hs_read(&host, f + 2, n - 2, buf, sizeof buf, &m)) return;
        accepted_sent = 1;
        if (mode == 0 && below(4) == 0) {   /* 受け入れの通知を壊す */
            char b[200];
            size_t bl = junk_lines(b, sizeof b - 0);
            host_data((uint8_t)rnd(), (uint16_t)below(3), (uint8_t)rnd(), b, bl > 150 ? 150 : bl);
            return;
        }
        host_data(OLDP_RESULT, 0, OLDP_RESPONSE, "STATUS=0\r\n", 10);
        if (mode == 1) {   /* ホストからの要求をすぐ送る */
            char b[4000];
            size_t bl = junk_lines(b, sizeof b);
            host_data((uint8_t)(below(2) ? OLDP_PING : rnd()), (uint16_t)rnd(), (uint8_t)(rnd() & 3), b, bl);
        }
    } else if (f[1] == OLDP_DATA) {
        if (!accepted_sent) op_before_accept = 1;
        if (oldp_cipher_decrypt(&host.recv, f, 2, f + 2, n - 2, buf, &m) || m < 4) return;
        if (buf[3] & OLDP_RESPONSE) return;   /* ホストの要求への機器の応答 */
        if (buf[3] & OLDP_MORE) return;       /* 続きは最後のフレームで答える */
        host_reply_to(buf, m);
    }
}

/* 機器が書いたバイトを、フレームごとに取り出す */
static int dev_write(void *ctx, const uint8_t *buf, size_t n) {
    (void)ctx;
    if (out_len + n > sizeof outb) out_len = 0;
    memcpy(outb + out_len, buf, n); out_len += n;
    for (;;) {
        if (transport == OLDP_TCP) {
            size_t fl;
            if (out_len < 2) break;
            fl = (size_t)(outb[0] << 8 | outb[1]);
            if (out_len < 2 + fl) break;
            host_frame(outb + 2, fl);
            memmove(outb, outb + 2 + fl, out_len - 2 - fl); out_len -= 2 + fl;
        } else {
            uint8_t *z = memchr(outb, 0, out_len);
            uint8_t u[70000];
            size_t un, cut;
            if (!z) break;
            cut = (size_t)(z - outb);
            un = oldp_cobs_decode(outb, cut, u, sizeof u);
            if (un != (size_t)-1 && un >= 6) {
                uint32_t crc = oldp_crc32(u, un - 4);
                if (((uint32_t)u[un - 4] << 24 | (uint32_t)u[un - 3] << 16 | (uint32_t)u[un - 2] << 8 | u[un - 1]) == crc && u[0] == 0) {
                    host_ack(u[1]);
                    host_frame(u + 2, un - 6);
                }
            }
            memmove(outb, outb + cut + 1, out_len - cut - 1); out_len -= cut + 1;
        }
    }
    return 0;
}
/* 積んだものを返す。無ければ時間切れ(すぐ返す) */
static int dev_read(void *ctx, uint8_t *buf, size_t n, uint32_t timeout_ms) {
    size_t k;
    (void)ctx; (void)timeout_ms;
    if (in_head == in_tail) return 0;
    k = in_tail - in_head;
    if (k > n) k = n;
    { size_t lim = 1 + below(600); if (k > lim) k = lim; }   /* 少しずつ届く */
    memcpy(buf, inq + in_head, k); in_head += k;
    return (int)k;
}

static int trust_all(void *app, const uint8_t pub[32], const char *fp, const char *caps, size_t len) { (void)app; (void)pub; (void)fp; (void)caps; (void)len; return 1; }
static oldp_job job;
static const char *const ACCEPT[] = { "JOB", "DBL", "HBOX", "TRCFMT", "R", "A", "ETYP", NULL };
static int on_line(void *ctx, const char *line, size_t len) { (void)ctx; oldp_job_line(&job, line, len); return 0; }
static int on_request(void *app, uint8_t op, const char *req, size_t len, char *resp, size_t cap, size_t *rl, uint8_t *rop) {
    (void)app; (void)req; (void)len; (void)cap;
    if (op == OLDP_AVAIL) { memcpy(resp, "STATUS=0\r\n", 10); *rl = 10; *rop = OLDP_RESULT; return 0; }
    return 1;
}

static oldp_dev dev;
static const char CAPS[] = "VER=1.0\r\nDEV=EDG\r\nVEN=FUZZ\r\nMODEL=F\r\nMAXFRAME=1024\r\nLABELS=JOB;DBL;TRCFMT;R\r\nTRACE=4;E\r\n";

int main(int argc, char **argv) {
    long sessions = argc > 1 ? atol(argv[1]) : 2000, s, done[3] = {0, 0, 0}, ok[3] = {0, 0, 0};
    uint8_t dev_priv[32];
    if (argc > 2) rs ^= (uint64_t)atoll(argv[2]) * 0x9e3779b97f4a7c15ULL;
    rbytes(dev_priv, 32); rbytes(host_priv, 32);

    /* oldp_cobs_decode と oldp_job_line を直接 */
    for (s = 0; s < 200000; s++) {
        uint8_t in[600], out[600];
        size_t n = below(600);
        rbytes(in, n);
        if (below(2)) for (size_t i = 0; i < n; i++) if (!in[i]) in[i] = 1;
        (void)oldp_cobs_decode(in, n, out, below(600));
    }
    for (s = 0; s < 20000; s++) {
        char b[8000];
        size_t bl = junk_lines(b, sizeof b), i = 0;
        oldp_job_init(&job, below(2) ? ACCEPT : NULL, below(2) ? "ACME" : NULL);
        while (i < bl) {
            size_t e = i;
            while (e < bl && b[e] != '\n') e++;
            oldp_job_line(&job, b + i, e - i);
            i = e + 1;
        }
        oldp_job_finish(&job);
        { char rep[512]; (void)oldp_job_result(&job, "N", rep, below(512)); }
    }

    for (s = 0; s < sessions; s++) {
        int r, st;
        uint8_t rop;
        char body[3000];
        size_t bl;
        mode = (int)(s % 3);
        transport = (s / 3) % 2 ? OLDP_UART : OLDP_TCP;
        in_head = in_tail = out_len = 0; host_seq = 0; accepted_sent = 0;
        oldp_dev_init(&dev, (oldp_io){ NULL, dev_write, dev_read, 0 }, (uint8_t)transport, dev_priv, CAPS, rng_fn);
        dev.trust = trust_all;
        dev.on_request = on_request;
        dev.timeout_ms = 10;
        dev.link.ack_timeout_ms = 1;
        dev.drop_unauth = transport == OLDP_UART;
        r = oldp_dev_open(&dev);
        done[mode]++;
        if (r) continue;
        if (!accepted_sent) { printf("NG 受け入れの通知を送る前に、機器が使える状態になった\n"); return 1; }
        ok[mode]++;
        r = oldp_dev_request(&dev, OLDP_TIME, NULL, 0, NULL, NULL, &rop, &st);
        if (!r) r = oldp_dev_request(&dev, OLDP_PING, NULL, 0, NULL, NULL, &rop, &st);
        bl = (size_t)snprintf(body, sizeof body, "JOB=F-%ld\r\nDBL=18.00\r\n", s);
        if (!r) r = oldp_dev_request(&dev, OLDP_PUT, body, bl, NULL, NULL, &rop, &st);
        oldp_job_init(&job, ACCEPT, NULL);
        if (!r) r = oldp_dev_request(&dev, OLDP_GET, "JOB=G\r\n", 7, on_line, NULL, &rop, &st);
        if (!r) {
            char rep[512];
            size_t rl;
            oldp_job_finish(&job);
            rl = oldp_job_result(&job, "G", rep, sizeof rep);
            r = oldp_dev_request(&dev, OLDP_RESULT, rep, rl, NULL, NULL, &rop, &st);
        }
        while (!r) r = oldp_dev_poll(&dev, 1);
    }
    if (op_before_accept) { printf("NG 受け入れの通知の前に、機器が操作を送った\n"); return 1; }
    printf("ハンドシェイクの前を壊す: %ld 回(使える状態になった %ld)\n", done[0], ok[0]);
    printf("ハンドシェイクの後を壊す: %ld 回(使える状態になった %ld)\n", done[1], ok[1]);
    printf("線の上のバイトを壊す:     %ld 回(使える状態になった %ld)\n", done[2], ok[2]);
    printf("メモリの誤り・未定義の動作は見つからなかった\n");
    return 0;
}
