/* 更新の仕組み(フラッシュの EB0 に置き、RAM で動く)。H8/3069F のユーザプログラムモードで本体(0x1000 番地から)を書き換える。
 * ハードウェアマニュアル 18.5.2 の手順に従い、チップ内蔵の消去・書き込みプログラムを RAM に取り出して呼ぶ。FWE 端子が 1 のときだけ書き換えられる。
 *
 * 起動: 本体の update コマンドから来たとき(印が立っている)は、すぐ更新の待ちに入る。
 *       リセットのときは 500 ms だけ "OLDPLOAD" を待ち、来なければ本体へ飛ぶ(本体が壊れていても、リセットボタンと PC の指示で書き戻せる)。
 * 手順(9600 bps。すべて PC から始める。答えは 'K'、失敗は 'N' と番号 1 バイト):
 *   'E' 長さ(4) CRC-32(4)              本体の範囲の消去
 *   'W' 番地(4) 中身(128) CRC-32(4)    128 バイトの書き込み(番地は 0x1000 以上で 128 バイト境界。CRC は番地と中身の 132 バイト)
 *                                      書いたあと読み直し、違えば 'N' 6 と、位置・読めた値・書いた値を返す
 *   'R' 番地(4) 長さ(2)                フラッシュの中身をそのまま返す(調べるため)
 *   'V'                                書いた範囲を読み直して CRC-32 を比べる
 *   'G'                                本体へ飛ぶ(答えは返さない) */
#include <stdint.h>
#include <stddef.h>
#include "../h8.h"
#include "../../src/oldp_cobs.h"

#define FCCS  REG8(0xfee0b0)
#define FPCS  REG8(0xfee0b1)
#define FECS  REG8(0xfee0b2)
#define FKEY  REG8(0xfee0b4)
#define FTDAR REG8(0xfee0b6)
#define FCCS_FWE 0x80

#define APP_BASE  0x1000UL
#define FLASH_END 0x80000UL
#define FTDAR_VAL 3                   /* ダウンロード先の指定(H'00=H'FFEF20、H'01=H'FFDF20、H'02=H'FFCF20、H'03=H'FFBF20) */
#define DLTOP     0xffbf20UL          /* FTDAR=3 のときのダウンロード先。更新の仕組みは 0xffd000 からなので重ならない */
#define MAGIC_ADDR (*(volatile uint32_t *)0xffff10)
#define MAGIC     0x55504454UL

#define FPEFEQ_20MHZ 2000UL           /* 動作周波数 20.00 MHz × 100 */

uint8_t flash_call(uint32_t er0, uint32_t er1, uint32_t entry);
void flash_sco(void);
void jump_app(void);

/* 内蔵の書き込みプログラムに渡すデータ。ワード単位で読まれても 1 バイトずれないよう、偶数番地に置く */
static uint8_t page[128] __attribute__((aligned(4)));

static void put(uint8_t c) {
    while (!(SCI1_SSR & SSR_TDRE)) ;
    SCI1_TDR = c;
    SCI1_SSR = (unsigned char)(SCI1_SSR & ~SSR_TDRE);
}
static void puts_(const char *s) { while (*s) put((uint8_t)*s++); }

static int get_now(void) {
    unsigned char s = SCI1_SSR, c;
    if (s & (SSR_ORER | SSR_FER | SSR_PER)) { SCI1_SSR = (unsigned char)(s & ~(SSR_ORER | SSR_FER | SSR_PER | SSR_RDRF)); return -1; }
    if (!(s & SSR_RDRF)) return -1;
    c = SCI1_RDR;
    SCI1_SSR = (unsigned char)(s & ~SSR_RDRF);
    return c;
}
static uint8_t get(void) { int c; while ((c = get_now()) < 0) ; return (uint8_t)c; }
static uint32_t get32(void) { uint32_t v = 0; uint8_t i; for (i = 0; i < 4; i++) v = (v << 8) | get(); return v; }
static void nak(uint8_t code) { put('N'); put(code); }

static void sci_init(void) {
    volatile uint16_t i;
    SCI1_SCR = 0;
    SCI1_SMR = 0;
    SCI1_BRR = 64;                    /* 20 MHz で 9600 bps */
    for (i = 0; i < 2000; i++) ;
    SCI1_SCR = SCR_TE | SCR_RE;
    SCI1_SSR = (unsigned char)(SCI1_SSR & ~(SSR_ORER | SSR_FER | SSR_PER));
}

/* 内蔵プログラムの取り出しと初期化(erase: 消去、0 なら書き込み)。戻りは 0 が成功 */
static uint8_t prepare(uint8_t erase) {
    volatile uint8_t *dl = (volatile uint8_t *)DLTOP;
    uint8_t r;
    FTDAR = FTDAR_VAL;
    if (erase) { FECS = 1; FPCS = 0; } else { FPCS = 1; FECS = 0; }
    *dl = 0xff;
    FKEY = 0xa5;
    flash_sco();
    FKEY = 0;
    if (*dl != 0) return (uint8_t)(0x10 | (*dl & 7));
    r = flash_call(FPEFEQ_20MHZ, 0, DLTOP + 32);
    return r ? (uint8_t)(0x20 | (r & 15)) : 0;
}

/* 消去ブロックの開始番地(図 18.4)。EB0 は更新の仕組みがいるので消さない */
static uint32_t block_start(uint8_t b) {
    if (b < 8) return (uint32_t)b * 0x1000UL;
    if (b == 8) return 0x8000UL;
    return 0x10000UL * (uint32_t)(b - 8);
}

static uint8_t erase_app(uint32_t len) {
    uint32_t end = APP_BASE + len;
    uint8_t b, r;
    if ((r = prepare(1))) return r;
    FKEY = 0x5a;
    for (b = 1; b < 16 && block_start(b) < end; b++) {
        r = flash_call(b, 0, DLTOP + 16);
        if (r) { FKEY = 0; return (uint8_t)(0x40 | (r & 15)); }
    }
    FKEY = 0;
    return prepare(0);                /* 続けて書き込みプログラムを取り出す */
}

static void update(void) {
    uint32_t len = 0, want = 0;
    puts_("LOAD\r\n");
    for (;;) {
        uint8_t c = get(), r;
        if (c == 'E') {
            len = get32();
            want = get32();
            if (!(FCCS & FCCS_FWE)) { nak(1); continue; }
            if (len == 0 || APP_BASE + len > FLASH_END) { nak(2); continue; }
            r = erase_app(len);
            if (r) nak(r); else put('K');
        } else if (c == 'W') {
            static uint8_t raw[132];
            uint32_t crc, addr;
            uint8_t i;
            for (i = 0; i < 132; i++) raw[i] = get();
            crc = get32();
            if (oldp_crc32(raw, 132) != crc) { nak(3); continue; }
            addr = ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) | ((uint32_t)raw[2] << 8) | raw[3];
            if (addr < APP_BASE || addr + 128 > FLASH_END || (addr & 0x7f)) { nak(4); continue; }
            for (i = 0; i < 128; i++) page[i] = raw[4 + i];
            FKEY = 0x5a;
            r = flash_call((uint32_t)page, addr, DLTOP + 16);
            FKEY = 0;
            if (r) { nak((uint8_t)(0x80 | (r & 0x7f))); continue; }
            /* 書いた 128 バイトをその場で読み直す。違えば 'N' 6 と、最初に食い違った位置・読めた値・書いた値を返す */
            for (i = 0; i < 128; i++) {
                uint8_t got = *(const volatile uint8_t *)(addr + i);
                if (got != page[i]) { nak(6); put(i); put(got); put(page[i]); break; }
            }
            if (i == 128) put('K');
        } else if (c == 'R') {
            /* 調べるため: 番地(4) 長さ(2) のフラッシュの中身をそのまま返す */
            uint32_t a = get32();
            uint16_t n = (uint16_t)((uint16_t)get() << 8);
            n |= get();
            while (n--) put(*(const volatile uint8_t *)a++);
        } else if (c == 'V') {
            if (len && oldp_crc32((const uint8_t *)APP_BASE, len) == want) put('K'); else nak(5);
        } else if (c == 'G') {
            jump_app();
        }
    }
}

int main(void) {
    static const char WORD[] = "OLDPLOAD";
    uint8_t m = 0;
    uint32_t ticks = 0;
    unsigned short last;
    sci_init();
    if (MAGIC_ADDR == MAGIC) { MAGIC_ADDR = 0; update(); }
    /* 500 ms のあいだ合図を待つ(φ/8 で 2500 カウントが 1 ms) */
    TSTR &= (unsigned char)~0x01;
    TCR0 = 0x03;
    TCNT0 = 0;
    TSTR |= 0x01;
    last = TCNT0;
    while (ticks < 1250000UL) {
        unsigned short t = TCNT0;
        int c = get_now();
        ticks += (unsigned short)(t - last);
        last = t;
        if (c < 0) continue;
        m = (c == WORD[m]) ? (uint8_t)(m + 1) : (uint8_t)(c == WORD[0]);
        if (!WORD[m]) update();
    }
    if (*(volatile uint32_t *)APP_BASE != 0xffffffffUL) jump_app();
    puts_("NOAPP\r\n");
    update();
    return 0;
}
