/* フラッシュの 1 ブロックを消して 128 バイト書く(ハードウェアマニュアル 18.5.2。手順は loader/loader.c と同じ)。
 * 消去と書き込みのあいだはフラッシュを読めないので、この手順は RAM(.data に一緒に写す .ramtext)に置く。
 * ここからフラッシュの中の定数・関数(libgcc を含む)を使わない。
 * 内蔵プログラムの取り出し先は 0xffdf20(FTDAR=1)。変数はそこより下にあり(rom.ld で確かめる)、
 * スタックは呼び出しのときその上にあるので重ならない。 */
#include "store.h"
#include "h8.h"

#define RAMFN __attribute__((section(".ramtext"), noinline))
#define FCCS  REG8(0xfee0b0)
#define FPCS  REG8(0xfee0b1)
#define FECS  REG8(0xfee0b2)
#define FKEY  REG8(0xfee0b4)
#define FTDAR REG8(0xfee0b6)
#define FCCS_FWE 0x80
#define FTDAR_VAL 1
#define DLTOP 0xffdf20UL
#define FPEFEQ_20MHZ 2000UL

uint8_t store_call(uint32_t er0, uint32_t er1, uint32_t entry);
void store_sco(void);

static RAMFN uint8_t prepare(uint8_t erase) {
    volatile uint8_t *dl = (volatile uint8_t *)DLTOP;
    FTDAR = FTDAR_VAL;
    if (erase) { FECS = 1; FPCS = 0; } else { FPCS = 1; FECS = 0; }
    *dl = 0xff;
    FKEY = 0xa5;
    store_sco();
    FKEY = 0;
    if (*dl != 0) return 0x10;
    return store_call(FPEFEQ_20MHZ, 0, DLTOP + 32) ? 0x20 : 0;
}

RAMFN uint8_t store_page(const uint8_t *page, uint32_t addr, uint8_t block) {
    uint8_t r;
    if (!(FCCS & FCCS_FWE)) return 1;
    if ((r = prepare(1))) return r;
    FKEY = 0x5a;
    r = store_call(block, 0, DLTOP + 16);
    FKEY = 0;
    if (r) return 0x40;
    if ((r = prepare(0))) return r;
    FKEY = 0x5a;
    r = store_call((uint32_t)page, addr, DLTOP + 16);
    FKEY = 0;
    return r ? 0x80 : 0;
}
