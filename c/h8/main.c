/* H8/3069F の基板で動く、Optical Lab Device Protocol の機器。内蔵フラッシュから起動する。
 * SCI1(既定は 9600 bps。8 ビット、パリティなし、ストップ 1)で、1 行ずつコマンドを受け付ける。
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
 *   baud [速度]    線の速度を見る・変える(9600・19200・38400・57600。フラッシュに残す)。"ok" を今の速度で返してから切り替える。
 *                  115200 も受け付けるが、20 MHz では実際の速度が 125000 bps(+8.5%)になり、規格の差の範囲(4 章)に合わない。
 *                  更新の仕組みは常に 9600 bps で動く
 *   rekey 1        機器の鍵を新しい鍵に替える(標準の拡張 OLDPKEY。ホストが受け付けたらフラッシュに残す)
 *   keyreset       書き込んだときの鍵に戻す
 *   bench          X25519 を 1 回計算する前後に "bench-start" "bench-end" を返す(時間は PC の側で測る)
 *   log            最後のセッションの結果を返す
 *   reset          起動の入口へ飛ぶ(更新の仕組みが 500 ms だけ合図を待つ)
 *   update         更新の仕組みへ移る。あとは PC の tools/h8-update.ps1 が本体を書き換える
 *   loader         更新の仕組み(EB0)を書き換える。"ready" を返したあと、長さ(4)・CRC-32(4)・中身を受け、EB0 を消して書き、
 *                  読み直して CRC-32 を比べる(PC の tools/h8-update.ps1 -Loader が使う)。書く間に電源が切れたら、ブートモードで書き直す
 *   rngtrace <回数>  試験用の組み立て(make trace)だけ。乱数に混ぜる文字の到着の時刻を記録して返す(tools/h8-rngtrace.ps1)
 *   rngboot <個数>   試験用の組み立てだけ。起動した直後からの到着の時刻を返す(再起動のテスト。tools/h8-rngtrace.ps1 -Restarts)
 *   xtime <種類> <鍵>  試験用の組み立てだけ。秘密の値を使う計算の時間を、サイクルの単位で返す(tools/h8-xtime.js)
 *   rngsess on|off・rngdump  試験用の組み立てだけ。実際のセッションの間に混ぜた値を記録して返す(tools/h8-rngsession.js)
 *   rnglan <回数>    試験用の組み立てだけ。LAN で受けたパケットで混ぜる値を記録して返す(tools/h8-rnglan.js)
 * put・get・ops・wait は "start" を返したあと、via uart では同じ線を UART のトランスポートとして使い(via lan なら線は結果の表示にだけ使う)、終わると少し待ってから
 * "OLDP-END <戻り値> <STATUS>" を返す(PC の中継は、これを見てホストとの間を切る)。
 * 乱数: 機器ごとの秘密の種(devkey.h)に、受け取った文字の到着の時刻を SHA-256 で混ぜて使う(規格 2 章)。
 * 受信: SCI1 の受信は割り込みで受け、リングバッファにためる(重い計算の間に届いた文字を落とさないため)。
 *       割り込みのベクタは更新の仕組み(EB0)にあり、RAM の 0xffff00(ERI1)・0xffff04(RXI1)へ飛ぶ。そこへ本体が飛び先を書く。
 *       ベクタがそうなっていない古い更新の仕組みでは、割り込みを使わずに読みに行く。 */
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
 * 文字を受けるのは割り込みの中なので、小さな配列に混ぜ込むだけにし、SHA-256 は乱数を取り出すときにまとめて行う */
static oldp_sha256_ctx pool;
static uint32_t rng_count;
static uint8_t ent[64];
static uint8_t ent_i;

#ifdef OLDP_RNG_TRACE
/* 試験用の組み立て(make trace)だけ: stir が受けた TCNT0 の下位 8 ビットを記録する。変数の領域に空きがないので、
 * 内蔵の書き込みプログラムの取り出し先(0xffdf20 から)を使う。rngtrace の間はフラッシュに書かないので重ならない。
 * スタック(0xffff00 から下へ)とは 4 KB 離れている */
#define TR_MAX 4096
#define TR_BUF ((volatile uint8_t *)0xffdf20UL)
#define TR_N   (*(volatile uint16_t *)0xffef20UL)   /* 記録した数。TR_MAX より大きいときは記録しない */
#define TR_FIRST (*(volatile uint16_t *)0xffef22UL) /* 起動してから最初に受けた文字の TCNT0(16 ビット) */
#define TR_SESS  (*(volatile uint8_t *)0xffef24UL)  /* 1 なら、セッションを始めるたびに記録を空にする(rngsess) */
#endif

void stir(uint8_t c) {
    unsigned short t = TCNT0;
#ifdef OLDP_RNG_TRACE
    { uint16_t n = TR_N; if (n < TR_MAX) { if (!n) TR_FIRST = t; TR_BUF[n] = (uint8_t)t; TR_N = (uint16_t)(n + 1); } }
#endif
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

/* ---- 受信 ----
 * 割り込みで受けて、リングバッファにためる。大きさは、内蔵 RAM の残り(変数は 0xffdf20 より下に置く。rom.ld)で決めた。
 * 1 つのフレームの全部(最大で約 520 バイト)は入らないが、フレームを処理する間に届く分を受けられればよい */
#define RXQ 384
#define TRAMP 0x00ffff00UL                    /* 割り込みの飛び先を書く RAM(ERI1 は +0、RXI1 は +4) */
/* 0 番地からのベクタ表を読む(定数の番地を直に読むと、gcc が配列の範囲外と警告するので、番地を asm で隠す) */
static uint32_t vector(uint8_t n) {
    const volatile uint32_t *p = 0;
    __asm__ ("" : "+r" (p));
    return p[n];
}
/* フラッシュの番地を指す(0 番地を含む) */
static const uint8_t *flash_at(uint32_t a) {
    const uint8_t *p = 0;
    __asm__ ("" : "+r" (p));
    return p + a;
}
static volatile uint8_t rxq[RXQ];
static volatile uint16_t rxq_head, rxq_tail;   /* head は割り込みが、tail は本体が進める */
static volatile uint16_t sci_err, rxq_lost;    /* SCI1 の受信の誤り(オーバーラン・フレーミング・パリティ)と、バッファがあふれて捨てた数。セッションごとに数え直す */
static uint8_t irq_on;

void sci1_isr(void) __attribute__((interrupt_handler));
void sci1_isr(void) {
    unsigned char s = SCI1_SSR;
    if (s & (SSR_ORER | SSR_FER | SSR_PER)) {
        sci_err++;
        SCI1_SSR = (unsigned char)(s & ~(SSR_ORER | SSR_FER | SSR_PER | SSR_RDRF));
        return;
    }
    if (s & SSR_RDRF) {
        uint8_t c = SCI1_RDR;
        uint16_t h = rxq_head, n = (uint16_t)(h + 1 == RXQ ? 0 : h + 1);
        SCI1_SSR = (unsigned char)(s & ~SSR_RDRF);
        if (n != rxq_tail) { rxq[h] = c; rxq_head = n; } else rxq_lost++;
        stir(c);
    }
}

/* 更新の仕組みのベクタが RAM の飛び先を指していれば、飛び先を書いて受信の割り込みを使う */
static void irq_init(void) {
    volatile uint8_t *t = (volatile uint8_t *)TRAMP;
    uint32_t a = (uint32_t)sci1_isr;
    uint8_t i;
    if (vector(56) != TRAMP || vector(57) != TRAMP + 4) return;   /* 56 番が ERI1、57 番が RXI1 */
    for (i = 0; i < 8; i += 4) {               /* JMP @aa:24 */
        t[i] = 0x5a; t[i + 1] = (uint8_t)(a >> 16); t[i + 2] = (uint8_t)(a >> 8); t[i + 3] = (uint8_t)a;
    }
    irq_on = 1;
    SCI1_SCR = (unsigned char)(SCI1_SCR | SCR_RIE);
    __asm__ volatile ("andc #0x7f,ccr");
}

/* フラッシュを消す・書く間は、ベクタ(フラッシュの中)を読めないので、割り込みを止める */
static uint8_t store_block_safe(const uint8_t *p, uint32_t addr, uint8_t block, uint16_t pages) {
    uint8_t r;
    __asm__ volatile ("orc #0x80,ccr");
    r = store_block(p, addr, block, pages);
    if (irq_on) __asm__ volatile ("andc #0x7f,ccr");
    return r;
}
static uint8_t store_safe(const uint8_t *p, uint32_t addr, uint8_t block) { return store_block_safe(p, addr, block, 1); }

static int getc_now(void) {
    unsigned char s, c;
    if (irq_on) {
        uint16_t t = rxq_tail;
        if (t == rxq_head) return -1;
        c = rxq[t];
        rxq_tail = (uint16_t)(t + 1 == RXQ ? 0 : t + 1);
        return c;
    }
    s = SCI1_SSR;
    if (s & (SSR_ORER | SSR_FER | SSR_PER)) { sci_err++; SCI1_SSR = (unsigned char)(s & ~(SSR_ORER | SSR_FER | SSR_PER | SSR_RDRF)); return -1; }
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
/* brr: 線の速度(SCI1 の BRR の値)。0xff は既定の 9600 bps */
typedef struct { uint8_t magic[4]; uint8_t count; uint8_t pad[3]; char hosts[HOST_MAX][20]; net_cfg net; uint8_t brr; uint8_t rest[15]; uint32_t crc; } host_page;   /* 128 バイト */
static host_page page __attribute__((aligned(4)));
static uint8_t pairing, dirty;
static uint32_t pair_until;
static const uint8_t MAGIC[4] = { 'V', 'H', '1', 0 };

static void hosts_load(void) {
    const host_page *p = (const host_page *)STORE_ADDR;
    if (!memcmp(p->magic, MAGIC, 4) && p->count <= HOST_MAX && oldp_crc32((const uint8_t *)p, 124) == p->crc) memcpy(&page, p, sizeof page);
    else { memset(&page, 0, sizeof page); memcpy(page.magic, MAGIC, 4); page.brr = 0xff; }
}

static void hosts_save(void) {
    memset(page.rest, 0xff, sizeof page.rest);
    page.crc = oldp_crc32((const uint8_t *)&page, 124);
    if (store_safe((const uint8_t *)&page, STORE_ADDR, STORE_BLOCK)) put_text("\r\nstore-failed");
    dirty = 0;
}

/* ---- 線の速度 ----
 * 20 MHz、φ/1 では、BRR = 20 MHz / (32 × 速度) - 1。実際の速度と差は、
 *   9600: 64 → 9615 bps(+0.16%)  19200: 32 → 18939 bps(-1.36%)  38400: 15 → 39063 bps(+1.73%)
 *   57600: 10 → 56818 bps(-1.36%)  115200: 4 → 125000 bps(+8.51%。規格 4 章の差の範囲に合わない) */
static const struct { uint32_t bps; uint8_t brr; } BAUDS[] = { { 9600, 64 }, { 19200, 32 }, { 38400, 15 }, { 57600, 10 }, { 115200, 4 } };
#define BRR_DEFAULT 64

static uint8_t brr_now(void) {
    uint8_t i;
    for (i = 0; i < sizeof BAUDS / sizeof BAUDS[0]; i++) if (BAUDS[i].brr == page.brr) return page.brr;
    return BRR_DEFAULT;
}

static uint32_t bps_of(uint8_t brr) {
    uint8_t i;
    for (i = 0; i < sizeof BAUDS / sizeof BAUDS[0]; i++) if (BAUDS[i].brr == brr) return BAUDS[i].bps;
    return 9600;
}

/* 送り終えてから(TEND)、速度を切り替える */
static void set_brr(uint8_t brr) {
    volatile uint32_t i;
    while (!(SCI1_SSR & SSR_TEND)) now_ms();
    SCI1_SCR = 0;
    SCI1_BRR = brr;
    for (i = 0; i < 2000; i++) ;      /* 1 ビットの時間より長く待つ */
    SCI1_SCR = (unsigned char)(SCR_TE | SCR_RE | (irq_on ? SCR_RIE : 0));
    SCI1_SSR = (unsigned char)(SCI1_SSR & ~(SSR_ORER | SSR_FER | SSR_PER));
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
    sci_err = 0; rxq_lost = 0;
    return oldp_dev_open(&dev);
}

/* PUT の中身を作る場所。loader コマンドでは、更新の仕組みの中身を受ける場所に使う(RAM の残りが少ないため) */
static char body[sizeof JOB_LINES + 64] __attribute__((aligned(4)));

static int op_put(const char *job, int *st) {
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
            if (store_safe((const uint8_t *)&kp, KEY_ADDR, KEY_BLOCK)) put_text("\r\nstore-failed");
            keys_load();
        }
        memset(&kp, 0, sizeof kp);
    }
    last_rc = r; last_status = st;
}

/* ---- 更新の仕組み(EB0)の書き換え ---- */
static int getc_wait(uint32_t t) {
    uint32_t s = now_ms();
    int c;
    while ((c = getc_now()) < 0) if (now_ms() - s >= t) return -1;
    return c;
}

static void do_loader(void) {
    uint8_t *buf = (uint8_t *)body, h[8];
    uint32_t len, crc, i, n;
    uint8_t r;
    put_text("ready\r\n");
    for (i = 0; i < 8; i++) { int c = getc_wait(3000); if (c < 0) { put_text("timeout"); return; } h[i] = (uint8_t)c; }
    len = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
    crc = ((uint32_t)h[4] << 24) | ((uint32_t)h[5] << 16) | ((uint32_t)h[6] << 8) | h[7];
    n = (len + 127) & ~127UL;
    if (len < 0x104 || n > 0x1000 || n > sizeof body) { put_text("bad-length"); return; }
    for (i = 0; i < len; i++) { int c = getc_wait(3000); if (c < 0) { put_text("timeout"); return; } buf[i] = (uint8_t)c; }
    if (oldp_crc32(buf, len) != crc) { put_text("bad-crc"); return; }
    /* リセットのベクタが起動の入口(0x100)を指していなければ、更新の仕組みではない */
    if (buf[0] || buf[1] || buf[2] != 0x01 || buf[3]) { put_text("not-loader"); return; }
    memset(buf + len, 0xff, n - len);
    r = store_block_safe(buf, 0, 0, (uint16_t)(n / 128));
    if (r) { put_text("store-failed "); put_dec(r); return; }
    put_text(oldp_crc32(flash_at(0), len) == crc ? "ok" : "verify-failed");
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

static void begin(const char *line) {
    memcpy(last_cmd, line, 4); last_cmd[4] = 0; if (last_cmd[3] == ' ') last_cmd[3] = 0;
#ifdef OLDP_RNG_TRACE
    if (TR_SESS == 1) TR_N = 0;   /* rngsess on: このセッションの間に混ぜた値を記録する */
#endif
    put_text("start\r\n");
}

static void end_session(void) {
    pairing = 0;
    if (via_lan) net_close();
    wait_ms(via_lan ? 0 : 500);
    if (dirty) hosts_save();
    put_text("\r\nOLDP-END "); put_dec(last_rc); put_text(" "); put_dec(last_status);
}

#ifdef OLDP_RNG_TRACE
/* rngtrace <回数>: PC が送る文字の並び(0x00 で区切る)を受け、区切りごとに 'A' を返す。記録が TR_MAX に達したら、
 * 'A' の代わりに 'D' と記録(TR_MAX バイト)を返す。これを <回数> だけ繰り返す(PC の tools/h8-rngtrace.ps1 が使う) */
static void do_rngtrace(uint32_t blocks) {
    uint16_t i;
    put_text("ready\r\n");
    TR_N = 0;
    while (blocks) {
        int c = getc_now();
        if (c < 0) { now_ms(); continue; }
        if (c) continue;
        if (TR_N < TR_MAX) { putc_wait('A'); continue; }
        TR_N = 0xffff;
        putc_wait('D');
        for (i = 0; i < TR_MAX; i++) putc_wait(TR_BUF[i]);
        if (--blocks) TR_N = 0;
    }
}

/* xtime <x25519|aead|hmac> <16 進の鍵 32 バイト>: 割り込みを止めて、秘密の値を使う計算を 1 回行い、かかった時間を
 * TCNT0 の数(φ/8 = 8 サイクルごと。65536 で一回りするので、26.2 ms で割った余り)で返す。
 * 計算の手順が値によって変わらなければ、どの鍵でも同じ数になる(この CPU にはキャッシュも分岐の予測もない)。
 *   x25519: 鍵を秘密の値として公開鍵を求める。aead: 鍵で 64 バイトを ChaCha20-Poly1305 で暗号化する。hmac: 鍵で 64 バイトの HMAC-SHA-256。
 *   aeadlong: 鍵で 480 バイトを ChaCha20-Poly1305 で暗号化する(Poly1305 の掛け算を増やし、小さな差を大きくして見るため) */
static void do_xtime(const char *arg) {
    uint8_t k[32], out[496], msg[480];
    const char *h = arg;
    uint16_t t0, t1, i;
    int kind = !memcmp(arg, "x25519 ", 7) ? 0 : !memcmp(arg, "aead ", 5) ? 1 : !memcmp(arg, "hmac ", 5) ? 2 : !memcmp(arg, "aeadlong ", 9) ? 3 : -1;
    if (kind < 0) { put_text("?"); return; }
    while (*h && *h != ' ') h++;
    h++;
    for (i = 0; i < 32; i++) {
        uint8_t v = 0, j;
        for (j = 0; j < 2; j++) {
            char c = *h++;
            v = (uint8_t)(v << 4 | (c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0'));
        }
        k[i] = v;
    }
    for (i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)i;
    __asm__ volatile ("orc #0x80,ccr");
    t0 = TCNT0;
    if (kind == 0) oldp_x25519_base(out, k);
    else if (kind == 1 || kind == 3) {
        oldp_cipher c;
        size_t n;
        memcpy(c.k, k, 32); c.has_key = 1; c.n = 0;
        oldp_cipher_encrypt(&c, msg, 2, msg, kind == 1 ? 64 : sizeof msg, out, &n);
    } else {
        oldp_hmac_ctx m;
        oldp_hmac_init(&m, k, 32); oldp_hmac_update(&m, msg, 64); oldp_hmac_final(&m, out);
    }
    t1 = TCNT0;
    if (irq_on) __asm__ volatile ("andc #0x7f,ccr");
    put_text("ticks="); put_dec((uint16_t)(t1 - t0));
}

/* rngsess on|off: on のあいだは、セッションを始めるたびに記録を空にして、セッションの間に混ぜた値を記録する。
 * rngdump: 記録した数(2 バイト、上位から)と記録を、'D' に続けて返し、記録を止める(PC の tools/h8-rngsession.js が使う) */
static void do_rngdump(void) {
    uint16_t i, n = TR_N > TR_MAX ? TR_MAX : TR_N;
    if (TR_N == 0xffff) n = 0;
    TR_N = 0xffff;
    putc_wait('D'); putc_wait((uint8_t)(n >> 8)); putc_wait((uint8_t)n);
    for (i = 0; i < n; i++) putc_wait(TR_BUF[i]);
}

/* rnglan <回数>: LAN で受けたパケットで混ぜる値を記録する。PC がこの基板へパケットを送り続ける間、受けて混ぜ、
 * 記録が TR_MAX に達するたびに 'D' と記録を返す。<回数> だけ繰り返す(PC の tools/h8-rnglan.js が使う) */
static void do_rnglan(uint32_t blocks) {
    uint16_t i;
    if (!net_is_up()) { put_text("no-lan"); return; }
    put_text("ready\r\n");
    TR_N = 0;
    while (blocks) {
        net_poll();
        if (TR_N < TR_MAX) continue;
        TR_N = 0xffff;
        putc_wait('D');
        for (i = 0; i < TR_MAX; i++) putc_wait(TR_BUF[i]);
        if (--blocks) TR_N = 0;
    }
}

/* rngboot <個数>: 起動してから記録した値を、<個数> になるまで rngtrace と同じ形で受け続け、'D'、最初の文字の TCNT0(2 バイト、
 * 上位から)、記録の先頭の <個数> バイトを返す(SP 800-90B の再起動のテスト。PC の tools/h8-rngtrace.ps1 -Restarts が使う) */
static void do_rngboot(uint32_t n) {
    uint16_t i, f;
    if (n < 1 || n > TR_MAX) { put_text("?"); return; }
    put_text("ready\r\n");
    for (;;) {
        int c = getc_now();
        if (c < 0) { now_ms(); continue; }
        if (c) continue;
        if (TR_N < n) { putc_wait('A'); continue; }
        TR_N = 0xffff;
        f = TR_FIRST;
        putc_wait('D'); putc_wait((uint8_t)(f >> 8)); putc_wait((uint8_t)f);
        for (i = 0; i < n; i++) putc_wait(TR_BUF[i]);
        return;
    }
}
#endif

#ifdef OLDP_UBSAN
/* 未定義の動作を確かめる組み立て(make ubsan)だけ。-fsanitize=undefined -fsanitize-trap=undefined で組み立てると、
 * 未定義の動作の手前で abort を呼ぶ。呼んだ場所の番地を返して止まる(tools/h8-ubsan.js が番地をソースの行に直す) */
void abort(void) {
    uint32_t a = (uint32_t)__builtin_return_address(0);
    uint8_t b[4];
    __asm__ volatile ("orc #0x80,ccr");
    b[0] = (uint8_t)(a >> 24); b[1] = (uint8_t)(a >> 16); b[2] = (uint8_t)(a >> 8); b[3] = (uint8_t)a;
    for (;;) { put_text("\r\nUBSAN-TRAP "); put_hex(b, 4); put_text("\r\n"); wait_ms(1000); }
}
#endif

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
    if (!via_lan) { put_text(" resent="); put_dec(dev.link.resent); put_text(" bad_units="); put_dec(dev.link.bad_units); put_text(" sci_err="); put_dec(sci_err); put_text(" rxq_lost="); put_dec(rxq_lost); put_text(" irq="); put_dec(irq_on); }
}

int main(void) {
    static char line[80];
    uint8_t i;
#ifdef OLDP_RNG_TRACE
    TR_N = 0;   /* 起動した直後から記録する(rngboot。rngtrace は数え直す) */
#endif
    board_init();
    hosts_load();
    irq_init();
    if (brr_now() != BRR_DEFAULT) set_brr(brr_now());
    keys_load();
    caps_build();
    if (page.net.set == 1) net_up(&page.net);
#if defined(OLDP_UBSAN)
    put_text("\r\noldp-h8 1.0 ubsan ");
#elif defined(OLDP_RNG_TRACE)
    put_text("\r\noldp-h8 1.0 rngtrace ");
#else
    put_text("\r\noldp-h8 1.0 ");
#endif
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
            put_text(store_safe((const uint8_t *)&kp, KEY_ADDR, KEY_BLOCK) ? "store-failed" : "ok");
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
        else if (!memcmp(line, "baud", 5)) { put_text("baud="); put_dec((int32_t)bps_of(brr_now())); put_text(" irq="); put_dec(irq_on); }
        else if (!memcmp(line, "loader", 7)) do_loader();
        else if (starts(line, "baud ")) {
            uint32_t v = to_u(line + 5);
            uint8_t i, b = 0;
            for (i = 0; i < sizeof BAUDS / sizeof BAUDS[0]; i++) if (BAUDS[i].bps == v) b = BAUDS[i].brr;
            if (!b) put_text("?");
            else {
                if (b != brr_now()) { page.brr = b == BRR_DEFAULT ? 0xff : b; hosts_save(); }
                put_text(v == 115200 ? "ok out-of-range" : "ok");
                set_brr(b);
            }
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
            __asm__ volatile ("orc #0x80,ccr");
            ((void (*)(void))0x100)();
        } else if (!memcmp(line, "update", 7)) {
            /* 更新の仕組みに「すぐ更新の待ちに入る」の印を渡し、割り込みを止めて飛ぶ */
            *(volatile uint32_t *)0xffff10 = 0x55504454UL;
            /* 更新の仕組みは 9600 bps で動く。PC が速度を合わせ直す間を置く */
            while (!(SCI1_SSR & SSR_TEND)) now_ms();
            wait_ms(200);
            __asm__ volatile ("orc #0x80,ccr");
            ((void (*)(void))0x100)();
        } else if (!memcmp(line, "log", 4)) put_log();
#ifdef OLDP_RNG_TRACE
        else if (starts(line, "rngtrace ")) do_rngtrace(to_u(line + 9));
        else if (starts(line, "rngboot ")) do_rngboot(to_u(line + 8));
        else if (starts(line, "xtime ")) do_xtime(line + 6);
        else if (!memcmp(line, "rngsess on", 11)) { TR_SESS = 1; TR_N = 0xffff; put_text("ok"); }
        else if (!memcmp(line, "rngsess off", 12)) { TR_SESS = 0; TR_N = 0xffff; put_text("ok"); }
        else if (!memcmp(line, "rngdump", 8)) do_rngdump();
        else if (starts(line, "rnglan ")) do_rnglan(to_u(line + 7));
#endif
        else put_text("?");
        put_text("\r\noldp> ");
    }
}
