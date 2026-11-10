/* H8/3069F の基板で動く、Optical Lab Device Protocol の機器。内蔵フラッシュから起動する。
 * SCI1(9600 bps、8 ビット、パリティなし、ストップ 1)で、1 行ずつコマンドを受け付ける。
 *   id             この機器の鍵のフィンガープリントを返す
 *   pair           ペアリングの受付(5 分、または次のセッションまで。知らないホストの鍵を覚える)
 *   hosts          覚えたホストの一覧
 *   unpair         覚えたホストをすべて忘れる
 *   trace <点数>   GET で受け取りたい玉型の点数(3-1440)
 *   put <JOB>      見本のジョブを PUT する
 *   get <名前>     ジョブを GET し、項目ごとの扱いを RESULT で返す
 *   ops <JOB> <名前>  1 つのセッションで TIME・PING・PUT・GET・RESULT・INFO を順に行う
 *   wait <秒>      セッションをつないだまま AVAIL を待ち、知らされたジョブを GET して RESULT と INFO を返す
 *   ip [IP マスク ゲートウェイ ホスト ポート]  LAN の設定を見る・書く(フラッシュに残す)
 *   via lan|uart   put・get・ops・wait のトランスポートを選ぶ(既定は uart)
 *   lan            LAN コントローラの状態と数を返す
 *   rekey 1        機器の鍵を新しい鍵に替える(標準の拡張 OLDPKEY。ホストが受け付けたらフラッシュに残す)
 *   keyreset       書き込んだときの鍵に戻す
 *   bench          X25519 を 1 回計算する前後に "bench-start" "bench-end" を返す(時間は PC の側で測る)
 *   log            最後のセッションの結果を返す
 *   reset          起動の入口へ飛ぶ(更新の仕組みが 500 ms だけ合図を待つ)
 *   update         更新の仕組みへ移る。あとは PC の tools/h8-update.ps1 が本体を書き換える
 * put・get・ops・wait は "start" を返したあと、via uart では同じ線を UART のトランスポートとして使い(via lan なら線は結果の表示にだけ使う)、終わると少し待ってから
 * "OLDP-END <戻り値> <STATUS>" を返す(PC の中継は、これを見てホストとの間を切る)。
 * 乱数: 機器ごとの秘密の種(devkey.h)に、受け取った文字の到着の時刻を SHA-256 で混ぜて使う(規格 2 章)。 */
#include <stdint.h>
#include <stddef.h>
#include "string.h"
#include "h8.h"
#include "oldp_device.h"
#include "oldp_job.h"
#include "oldp_cobs.h"
#include "oldp_sha256.h"
#include "oldp_x25519.h"
#include "store.h"
#include "rtl8019.h"
#include "net.h"
#include "devkey.h"
#include "job.h"

#define TICKS_PER_MS 2500   /* φ = 20 MHz の φ/8 */
#define PAIR_WINDOW_MS 300000UL   /* spec/messages.json の pairing.windowMs */

/* ---- 時計と線 ---- */
static unsigned short last_tick;
static uint32_t acc, ms;

uint32_t now_ms(void) {
    unsigned short c = TCNT0;
    acc += (unsigned short)(c - last_tick);
    last_tick = c;
    while (acc >= TICKS_PER_MS) { acc -= TICKS_PER_MS; ms++; }
    return ms;
}

/* ---- 乱数: 秘密の種に、文字の到着の時刻を混ぜる ----
 * 受けるたびに SHA-256 を計算すると、H8 では 1 文字の間隔(9600 bps で約 1 ms)より長くかかり、SCI1 の受け場所が
 * あふれて文字が落ちる。受けたときは小さな配列に混ぜ込むだけにし、SHA-256 は乱数を取り出すときにまとめて行う */
static oldp_sha256_ctx pool;
static uint32_t rng_count;
static uint8_t ent[64];
static uint8_t ent_i;

void stir(uint8_t c) {
    unsigned short t = TCNT0;
    ent[ent_i & 63] ^= (uint8_t)(c ^ (uint8_t)t);
    ent[(ent_i + 1) & 63] ^= (uint8_t)(t >> 8);
    ent_i = (uint8_t)(ent_i + 2);
}

static void rng(uint8_t *buf, size_t n) {
    oldp_sha256_update(&pool, ent, sizeof ent);
    oldp_sha256_update(&pool, &ent_i, 1);
    while (n) {
        oldp_sha256_ctx c = pool;
        uint8_t d[32], k[4];
        size_t take = n < 32 ? n : 32;
        unsigned short t = TCNT0;
        rng_count++;
        k[0] = (uint8_t)rng_count; k[1] = (uint8_t)(rng_count >> 8); k[2] = (uint8_t)t; k[3] = (uint8_t)(t >> 8);
        oldp_sha256_update(&c, k, 4);
        oldp_sha256_final(&c, d);
        memcpy(buf, d, take);
        buf += take; n -= take;
        oldp_sha256_update(&pool, d, 32);   /* 出した値をたまりに戻し、同じ値を 2 度出さない */
    }
}

static int getc_now(void) {
    unsigned char s = SCI1_SSR, c;
    if (s & (SSR_ORER | SSR_FER | SSR_PER)) { SCI1_SSR = (unsigned char)(s & ~(SSR_ORER | SSR_FER | SSR_PER | SSR_RDRF)); return -1; }
    if (!(s & SSR_RDRF)) return -1;
    c = SCI1_RDR;
    SCI1_SSR = (unsigned char)(s & ~SSR_RDRF);
    stir(c);
    return c;
}

static void putc_wait(uint8_t c) {
    while (!(SCI1_SSR & SSR_TDRE)) now_ms();
    SCI1_TDR = c;
    SCI1_SSR = (unsigned char)(SCI1_SSR & ~SSR_TDRE);
}

static void put_text(const char *s) { while (*s) putc_wait((uint8_t)*s++); }

static void put_dec(int32_t v) {
    char b[12];
    int i = 0;
    if (v < 0) { putc_wait('-'); v = -v; }
    do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) putc_wait((uint8_t)b[--i]);
}

static void put_hex(const uint8_t *p, size_t n) {
    static const char H[] = "0123456789abcdef";
    while (n--) { putc_wait((uint8_t)H[*p >> 4]); putc_wait((uint8_t)H[*p & 15]); p++; }
}

/* oldp_io: 書く */
static int io_write(void *ctx, const uint8_t *buf, size_t n) { (void)ctx; while (n--) putc_wait(*buf++); return 0; }

/* oldp_io: 読む。1 バイト以上読めたらその数、timeout_ms のあいだ何も来なければ 0 */
static int io_read(void *ctx, uint8_t *buf, size_t n, uint32_t timeout_ms) {
    uint32_t start = now_ms();
    size_t got = 0;
    (void)ctx;
    while (got < n) {
        int c = getc_now();
        if (c >= 0) { buf[got++] = (uint8_t)c; continue; }
        if (got) break;
        if (now_ms() - start >= timeout_ms) return 0;
    }
    return (int)got;
}

/* ---- ペアリング(覚えたホストはフラッシュの最後のブロックに残す) ---- */
#define HOST_MAX 4
#define STORE_ADDR 0x70000UL
#define STORE_BLOCK 15
typedef struct { uint8_t magic[4]; uint8_t count; uint8_t pad[3]; char hosts[HOST_MAX][20]; net_cfg net; uint8_t rest[16]; uint32_t crc; } host_page;   /* 128 バイト */
static host_page page __attribute__((aligned(4)));
static uint8_t pairing, dirty;
static uint32_t pair_until;
static const uint8_t MAGIC[4] = { 'V', 'H', '1', 0 };

static void hosts_load(void) {
    const host_page *p = (const host_page *)STORE_ADDR;
    if (!memcmp(p->magic, MAGIC, 4) && p->count <= HOST_MAX && oldp_crc32((const uint8_t *)p, 124) == p->crc) memcpy(&page, p, sizeof page);
    else { memset(&page, 0, sizeof page); memcpy(page.magic, MAGIC, 4); }
}

static void hosts_save(void) {
    memset(page.rest, 0xff, sizeof page.rest);
    page.crc = oldp_crc32((const uint8_t *)&page, 124);
    if (store_page((const uint8_t *)&page, STORE_ADDR, STORE_BLOCK)) put_text("\r\nstore-failed");
    dirty = 0;
}

static int trust(void *app, const uint8_t pub[32], const char *fp, const char *caps, size_t len) {
    uint8_t i;
    (void)app; (void)pub; (void)caps; (void)len;
    for (i = 0; i < page.count; i++) if (!memcmp(page.hosts[i], fp, 19)) return 1;
    if (!pairing || (int32_t)(pair_until - now_ms()) <= 0 || page.count >= HOST_MAX) return 0;
    memcpy(page.hosts[page.count++], fp, 20);
    dirty = 1;
    return 1;
}

/* ---- ケイパビリティ ---- */
static char caps[256];
static uint16_t trace_points = 360;

static void caps_build(void) {
    static const char A[] = "VER=1.0\r\nDEV=EDG\r\nVEN=H8\r\nMODEL=3069F\r\nSN=H8-0001\r\nMAXFRAME=512\r\n"
        "LABELS=JOB;DO;DBL;HBOX;VBOX;CIRC;FCRV;TRCFMT;R;A\r\nLABELS=ETYP;BEVP;BEVM;BEVC;POLISH;LIND;SPH;CYL;AX;IPD;OCHT\r\nEXT=OLDPKEY\r\nTRACE=";
    size_t n = sizeof A - 1;
    char d[6];
    int i = 0;
    uint16_t v = trace_points;
    memcpy(caps, A, n);
    do { d[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) caps[n++] = d[--i];
    memcpy(caps + n, ";E\r\n", 5);
}

static void board_init(void) {
    volatile uint32_t i;
    /* SCI1: 調歩同期、8 ビット、パリティなし、ストップ 1、φ/1。BRR = 20 MHz / (32 × 9600) - 1 ≒ 64 */
    SCI1_SCR = 0;
    SCI1_SMR = 0;
    SCI1_BRR = 64;
    for (i = 0; i < 2000; i++) ;      /* 1 ビットの時間より長く待つ */
    SCI1_SCR = SCR_TE | SCR_RE;
    SCI1_SSR = (unsigned char)(SCI1_SSR & ~(SSR_ORER | SSR_FER | SSR_PER));
    /* 16 ビットタイマ チャネル 0: φ/8 で数え続ける */
    TSTR &= (unsigned char)~0x01;
    TCR0 = 0x03;
    TCNT0 = 0;
    TSTR |= 0x01;
    oldp_sha256_init(&pool);
    oldp_sha256_update(&pool, DEVICE_SEED, sizeof DEVICE_SEED);
}

static oldp_dev dev;
static const char *const ACCEPT[] = { "JOB", "DO", "DBL", "HBOX", "VBOX", "CIRC", "FCRV", "TRCFMT", "R", "A", "ETYP", "BEVP", "BEVM", "BEVC", "POLISH", "LIND", "SPH", "CYL", "AX", "IPD", "OCHT", 0 };
static oldp_job got;
static int on_line(void *ctx, const char *line, size_t len) { (void)ctx; oldp_job_line(&got, line, len); return 0; }

/* ---- 最後のセッションの結果 ---- */
static int32_t last_rc, last_status = -1, put_bytes, r_time, r_ping, r_put, r_get, r_result, r_info, pings;
static char last_cmd[8], avail_job[44], host_time[16];

static void wait_ms(uint32_t t) { uint32_t s = now_ms(); while (now_ms() - s < t) getc_now(); }

/* ホストからの要求: AVAIL は名前を覚える。PING は数えて答える */
static int on_request(void *app, uint8_t op, const char *body, size_t len, char *resp, size_t cap, size_t *rl, uint8_t *rop) {
    (void)app; (void)cap;
    if (op == OLDP_AVAIL && len > 4 && !memcmp(body, "JOB=", 4)) {
        size_t n = 4;
        while (n < len && body[n] != '\r' && body[n] != '\n') n++;
        if (n - 4 < sizeof avail_job && !avail_job[0]) { memcpy(avail_job, body + 4, n - 4); avail_job[n - 4] = 0; }
        memcpy(resp, "STATUS=0\r\n", 10); *rl = 10; *rop = OLDP_RESULT;
        return 0;
    }
    if (op == OLDP_PING) { pings++; *rl = 0; *rop = OLDP_PING; return 0; }
    return 1;
}

/* ---- 機器の鍵(交換した鍵はフラッシュの EB14 に残す。本体の更新では消えない) ---- */
#define KEY_ADDR 0x60000UL
#define KEY_BLOCK 14
#define OP_KEY_CHANGE 0x80
typedef struct { uint8_t magic[4]; uint8_t priv[32], pub[32]; char fp[20]; uint8_t rest[36]; uint32_t crc; } key_page;   /* 128 バイト */
static key_page kp __attribute__((aligned(4)));
static const uint8_t KEY_MAGIC[4] = { 'V', 'K', '1', 0 };
static const uint8_t *my_priv = DEVICE_KEY, *my_pub = DEVICE_PUB;
static const char *my_fp = DEVICE_FP;

static void keys_load(void) {
    const key_page *p = (const key_page *)KEY_ADDR;
    if (!memcmp(p->magic, KEY_MAGIC, 4) && oldp_crc32((const uint8_t *)p, 124) == p->crc) { my_priv = p->priv; my_pub = p->pub; my_fp = p->fp; }
    else { my_priv = DEVICE_KEY; my_pub = DEVICE_PUB; my_fp = DEVICE_FP; }
}

static uint8_t via_lan;
static int last_net;

static int session_open(void) {
    if (via_lan) {
        uint8_t seed[6];
        int r;
        oldp_dev_init(&dev, (oldp_io){ 0, net_write, net_read, net_flush }, OLDP_TCP, my_priv, caps, rng);
        if (!net_is_up()) { last_net = NET_E_NOLAN; return OLDP_E_IO; }
        rng(seed, sizeof seed);
        /* TCP では UART の作業場所を使わないので、受信のバッファにする */
        r = net_connect(dev.link.pend, sizeof dev.link.pend + sizeof dev.link.raw, seed);
        last_net = r;
        if (r) return OLDP_E_IO;
    } else oldp_dev_init(&dev, (oldp_io){ 0, io_write, io_read, 0 }, OLDP_UART, my_priv, caps, rng);
    dev.trust = trust;
    dev.on_request = on_request;
    dev.s_pub = my_pub;
    avail_job[0] = 0;
    return oldp_dev_open(&dev);
}

static int op_put(const char *job, int *st) {
    static char body[sizeof JOB_LINES + 64];
    size_t n = 0, k = strlen(job);
    uint8_t rop;
    memcpy(body, "JOB=", 4); n = 4;
    memcpy(body + n, job, k); n += k;
    memcpy(body + n, "\r\n", 2); n += 2;
    memcpy(body + n, JOB_LINES, sizeof JOB_LINES - 1); n += sizeof JOB_LINES - 1;
    put_bytes = (int32_t)n;
    return oldp_dev_request(&dev, OLDP_PUT, body, n, 0, 0, &rop, st);
}

/* GET して、項目ごとの扱いを RESULT で返す */
static int op_get(const char *name, int *st) {
    static char req[64], rep[256];
    size_t k = strlen(name), n;
    uint8_t rop = 0;
    int r;
    oldp_job_init(&got, ACCEPT, 0);
    memcpy(req, "JOB=", 4); memcpy(req + 4, name, k); memcpy(req + 4 + k, "\r\n", 2);
    r = oldp_dev_request(&dev, OLDP_GET, req, k + 6, on_line, 0, &rop, st);
    r_get = r;
    if (r || rop != OLDP_DATA_OP) return r;
    oldp_job_finish(&got);
    n = oldp_job_result(&got, name, rep, sizeof rep);
    r_result = r = oldp_dev_request(&dev, OLDP_RESULT, rep, n, 0, 0, &rop, st);
    return r;
}

static int time_line(void *ctx, const char *line, size_t len) {
    (void)ctx;
    if (len == 19 && !memcmp(line, "TIME=", 5)) { memcpy(host_time, line + 5, 14); host_time[14] = 0; }
    return 0;
}

static int op_info(const char *job, int *st) {
    static char b[64];
    size_t k = strlen(job);
    uint8_t rop;
    memcpy(b, "JOB=", 4); memcpy(b + 4, job, k); memcpy(b + 4 + k, "\r\nEVENT=DONE\r\n", 14);
    return oldp_dev_request(&dev, OLDP_INFO, b, k + 18, 0, 0, &rop, st);
}

static void do_put(const char *job) {
    int st = -1, r = session_open();
    if (!r) r = op_put(job, &st);
    r_put = r; last_rc = r; last_status = st;
}

static void do_get(const char *name) {
    int st = -1, r = session_open();
    if (!r) r = op_get(name, &st);
    last_rc = r; last_status = st;
}

static void do_ops(const char *job, const char *name) {
    int st = -1, st2 = -1, r = session_open();
    uint8_t rop;
    r_time = r_ping = r_put = r_get = r_result = r_info = -99;
    host_time[0] = 0;
    if (!r) {
        r_time = r = oldp_dev_request(&dev, OLDP_TIME, 0, 0, time_line, 0, &rop, &st2);
        if (!r) r_ping = r = oldp_dev_request(&dev, OLDP_PING, 0, 0, 0, 0, &rop, &st2);
        if (!r) { r_put = r = op_put(job, &st); if (!r && st) r_put = st; }
        if (!r) r = op_get(name, &st);
        if (!r) r_info = r = op_info(name, &st2);
    }
    last_rc = r; last_status = st;
}

/* AVAIL を待って、知らされたジョブを取りに行く */
static void do_wait(uint32_t seconds) {
    int st = -1, st2 = -1, r = session_open();
    uint32_t start = now_ms(), lim = seconds * 1000UL;
    r_get = r_result = r_info = -99;
    while (!r && !avail_job[0]) {
        if (now_ms() - start >= lim) { r = OLDP_E_TIMEOUT; break; }
        r = oldp_dev_poll(&dev, 1000);
        if (r == OLDP_E_TIMEOUT) r = 0;
    }
    if (!r) r = op_get(avail_job, &st);
    if (!r) r_info = r = op_info(avail_job, &st2);
    last_rc = r; last_status = st;
}

/* 新しい鍵を作ってホストに知らせ、受け付けられたらフラッシュに残す */
static void do_rekey(void) {
    static const char H[] = "0123456789abcdef";
    static char body[96];
    int r = session_open(), st = -1;
    uint8_t rop, i;
    if (!r) {
        size_t n = 0;
        rng(kp.priv, 32);
        oldp_x25519_base(kp.pub, kp.priv);
        memcpy(body, "EXT=OLDPKEY\r\nKEY=", 17); n = 17;
        for (i = 0; i < 32; i++) { body[n++] = H[kp.pub[i] >> 4]; body[n++] = H[kp.pub[i] & 15]; }
        body[n++] = '\r'; body[n++] = '\n';
        r = oldp_dev_request(&dev, OP_KEY_CHANGE, body, n, 0, 0, &rop, &st);
        if (!r && st == 0) {
            memcpy(kp.magic, KEY_MAGIC, 4);
            oldp_fingerprint(kp.pub, kp.fp);
            memset(kp.rest, 0xff, sizeof kp.rest);
            kp.crc = oldp_crc32((const uint8_t *)&kp, 124);
            if (store_page((const uint8_t *)&kp, KEY_ADDR, KEY_BLOCK)) put_text("\r\nstore-failed");
            keys_load();
        }
        memset(&kp, 0, sizeof kp);
    }
    last_rc = r; last_status = st;
}

static int get_line(char *buf, int cap) {
    int n = 0, c;
    for (;;) {
        now_ms();
        net_poll();
        c = getc_now();
        if (c < 0) continue;
        if (c == '\r' || c == '\n') { if (n > 0) break; continue; }
        if (n < cap - 1) buf[n++] = (char)c;
    }
    buf[n] = 0;
    return n;
}

static int starts(const char *s, const char *p) { return !memcmp(s, p, strlen(p)); }
static uint32_t to_u(const char *s) { uint32_t v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (uint32_t)(*s++ - '0'); return v; }

static void put_ip(const uint8_t *a) {
    uint8_t i;
    for (i = 0; i < 4; i++) { if (i) putc_wait('.'); put_dec(a[i]); }
}

/* "1.2.3.4 " を読んで次の項目へ進む */
static int parse_ip(const char **ps, uint8_t ip[4]) {
    const char *q = *ps;
    uint8_t i;
    for (i = 0; i < 4; i++) {
        uint16_t v = 0, d = 0;
        while (*q >= '0' && *q <= '9' && d < 4) { v = (uint16_t)(v * 10 + (uint16_t)(*q++ - '0')); d++; }
        if (!d || v > 255) return 0;
        ip[i] = (uint8_t)v;
        if (i < 3) { if (*q != '.') return 0; q++; }
    }
    if (*q != ' ') return 0;
    *ps = q + 1;
    return 1;
}

static void begin(const char *line) { memcpy(last_cmd, line, 4); last_cmd[4] = 0; if (last_cmd[3] == ' ') last_cmd[3] = 0; put_text("start\r\n"); }

static void end_session(void) {
    pairing = 0;
    if (via_lan) net_close();
    wait_ms(via_lan ? 0 : 500);
    if (dirty) hosts_save();
    put_text("\r\nOLDP-END "); put_dec(last_rc); put_text(" "); put_dec(last_status);
}

static void put_log(void) {
    put_text(last_cmd); put_text(" rc="); put_dec(last_rc); put_text(" status="); put_dec(last_status);
    if (last_cmd[0] == 'p') { put_text(" bytes="); put_dec(put_bytes); }
    if (last_cmd[0] == 'o') {
        put_text(" time="); put_text(host_time); put_text(" r_time="); put_dec(r_time); put_text(" r_ping="); put_dec(r_ping);
        put_text(" r_put="); put_dec(r_put); put_text(" bytes="); put_dec(put_bytes);
    }
    if (last_cmd[0] == 'o' || last_cmd[0] == 'w') { put_text(" r_info="); put_dec(r_info); put_text(" pings="); put_dec(pings); }
    if (last_cmd[0] == 'w') { put_text(" avail="); put_text(avail_job); }
    if (last_cmd[0] != 'p') {
        put_text(" r_get="); put_dec(r_get); put_text(" r_result="); put_dec(r_result);
        put_text(" lines="); put_dec(got.lines); put_text(" traces="); put_dec(got.traces);
        put_text(" bad="); put_dec(got.bad_trace); put_text(" ignored="); put_text(got.ignored); put_text(" rejected="); put_text(got.rejected);
    }
    put_text(" fail="); put_dec(dev.last_fail); put_text(" host="); put_text(dev.peer_fp);
}

int main(void) {
    static char line[80];
    uint8_t i;
    board_init();
    hosts_load();
    keys_load();
    caps_build();
    if (page.net.set == 1) net_up(&page.net);
    put_text("\r\noldp-h8 1.0 ");
    put_text(my_fp);
    put_text("\r\noldp> ");
    for (;;) {
        get_line(line, sizeof line);
        if (!memcmp(line, "id", 3)) { put_text("ID "); put_text(my_fp); }
        else if (!memcmp(line, "pair", 5)) { pairing = 1; pair_until = now_ms() + PAIR_WINDOW_MS; put_text("ok"); }
        else if (!memcmp(line, "hosts", 6)) {
            put_text("hosts="); put_dec(page.count);
            for (i = 0; i < page.count; i++) { put_text(" "); put_text(page.hosts[i]); }
        }
        else if (!memcmp(line, "unpair", 7)) { page.count = 0; memset(page.hosts, 0, sizeof page.hosts); hosts_save(); put_text("ok"); }
        else if (starts(line, "trace ")) {
            uint32_t v = to_u(line + 6);
            if (v >= 3 && v <= 1440) { trace_points = (uint16_t)v; caps_build(); put_text("ok"); } else put_text("?");
        }
        else if (starts(line, "put ")) { begin(line); do_put(line + 4); end_session(); }
        else if (starts(line, "get ")) { begin(line); do_get(line + 4); end_session(); }
        else if (starts(line, "ops ")) {
            char *sp = line + 4;
            while (*sp && *sp != ' ') sp++;
            if (*sp) { *sp = 0; begin(line); do_ops(line + 4, sp + 1); end_session(); } else put_text("?");
        }
        else if (starts(line, "wait ")) { begin(line); do_wait(to_u(line + 5)); end_session(); }
        else if (starts(line, "rekey ")) { begin(line); do_rekey(); end_session(); }
        else if (!memcmp(line, "keyreset", 9)) {
            memset(&kp, 0, sizeof kp);
            put_text(store_page((const uint8_t *)&kp, KEY_ADDR, KEY_BLOCK) ? "store-failed" : "ok");
            keys_load();
        }
        else if (!memcmp(line, "lan", 4)) {
            put_text("up="); put_dec(net_is_up());
            put_text(" mac="); put_hex(net_mac(), 6);
            put_text(" rx="); put_dec((int32_t)net_stats.rx); put_text(" tx="); put_dec((int32_t)net_stats.tx);
            put_text(" retrans="); put_dec((int32_t)net_stats.retrans); put_text(" badsum="); put_dec((int32_t)net_stats.bad_csum);
            put_text(" over="); put_dec((int32_t)net_stats.overflow); put_text(" last="); put_dec(last_net); put_text(" err="); put_dec(net_last_error());
            put_text(" via="); put_text(via_lan ? "lan" : "uart");
        }
        else if (!memcmp(line, "via lan", 8)) { via_lan = 1; put_text("ok"); }
        else if (!memcmp(line, "via uart", 9)) { via_lan = 0; put_text("ok"); }
        else if (!memcmp(line, "ip", 3)) {
            if (page.net.set != 1) put_text("unset");
            else {
                put_ip(page.net.ip); putc_wait(' '); put_ip(page.net.mask); putc_wait(' '); put_ip(page.net.gw); putc_wait(' ');
                put_ip(page.net.host); putc_wait(' '); put_dec(page.net.port);
            }
        }
        else if (starts(line, "ip ")) {
            const char *q = line + 3;
            net_cfg c;
            uint32_t pt = 0;
            memset(&c, 0, sizeof c);
            if (parse_ip(&q, c.ip) && parse_ip(&q, c.mask) && parse_ip(&q, c.gw) && parse_ip(&q, c.host) && (pt = to_u(q)) > 0 && pt < 65536) {
                c.set = 1;
                c.port = (uint16_t)pt;
                page.net = c;
                hosts_save();
                put_text(net_up(&page.net) ? "no-lan" : "ok");
            } else put_text("?");
        }
        else if (!memcmp(line, "bench", 6)) {
            uint8_t a[32], b[32];
            rng(a, 32);
            put_text("bench-start\r\n");
            oldp_x25519_base(b, a);
            put_text("bench-end");
        } else if (!memcmp(line, "reset", 6)) {
            ((void (*)(void))0x100)();
        } else if (!memcmp(line, "update", 7)) {
            /* 更新の仕組みに「すぐ更新の待ちに入る」の印を渡して飛ぶ。割り込みは使っていない */
            *(volatile uint32_t *)0xffff10 = 0x55504454UL;
            ((void (*)(void))0x100)();
        } else if (!memcmp(line, "log", 4)) put_log();
        else put_text("?");
        put_text("\r\noldp> ");
    }
}
