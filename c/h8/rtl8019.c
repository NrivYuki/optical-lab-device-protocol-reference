#include "h8.h"
#include "rtl8019.h"

#define R(n) REG8(0x200000UL + (n))

/* どのページでも同じ */
#define CR    R(0x00)
#define RDMAP R(0x10)
#define RSTP  R(0x18)
/* ページ 0 */
#define PSTART R(0x01)
#define PSTOP  R(0x02)
#define BNRY   R(0x03)
#define TPSR   R(0x04)
#define TBCR0  R(0x05)
#define TBCR1  R(0x06)
#define ISR    R(0x07)
#define RSAR0  R(0x08)
#define RSAR1  R(0x09)
#define ID0    R(0x0a)   /* 読むとき */
#define ID1    R(0x0b)   /* 読むとき */
#define RBCR0  R(0x0a)   /* 書くとき */
#define RBCR1  R(0x0b)   /* 書くとき */
#define RCR    R(0x0c)
#define TCR    R(0x0d)
#define DCR    R(0x0e)
#define IMR    R(0x0f)
/* ページ 1 */
#define PAR(i) R(0x01 + (i))
#define CURR   R(0x07)
#define MAR(i) R(0x08 + (i))

#define CR_STOP  0x01
#define CR_START 0x02
#define CR_TXP   0x04
#define CR_READ  0x08
#define CR_WRITE 0x10
#define CR_ABORT 0x20
#define CR_PAGE1 0x40

#define ISR_OVW 0x10
#define ISR_RDC 0x40
#define ISR_RST 0x80

/* 受信の領域。8 ビットのつなぎ方でも確実に使える 0x4000-0x5FFF の中に置く */
#define RX_START 0x48
#define RX_STOP  0x60

#define RCR_BROADCAST 0x04

static uint8_t rx_next;

void rtl_bus_init(void) {
    /* A0-A7 を出力、CS1 と CS2 を出力にする(モード 5。エリア 1 は ABWCR の初期値で 8 ビット、ウェイトは初期値の 3) */
    P1DDR = 0xff;
    P8DDR = 0xec;
}

static void delay(uint32_t n) { volatile uint32_t i; for (i = 0; i < n; i++) ; }

int rtl_probe(uint8_t id[2]) {
    uint16_t i;
    uint8_t v = RSTP;
    int ok = 0;
    RSTP = v;
    for (i = 0; i < 20000; i++) if (ISR & ISR_RST) { ok = 1; break; }
    CR = CR_ABORT | CR_STOP;
    delay(2000);
    id[0] = ID0;
    id[1] = ID1;
    return ok;
}

static void dma_setup(uint16_t addr, uint16_t n, uint8_t cmd) {
    CR = CR_ABORT | CR_START;
    RBCR0 = (uint8_t)n;
    RBCR1 = (uint8_t)(n >> 8);
    RSAR0 = (uint8_t)addr;
    RSAR1 = (uint8_t)(addr >> 8);
    CR = cmd | CR_START;
}

static void dma_wait(void) {
    uint16_t i;
    for (i = 0; i < 20000; i++) if (ISR & ISR_RDC) break;
    ISR = ISR_RDC;
}

static void dma_read(uint16_t addr, uint8_t *buf, uint16_t n) {
    if (!n) return;
    dma_setup(addr, n, CR_READ);
    while (n--) *buf++ = RDMAP;
    dma_wait();
}

void rtl_read_prom(uint8_t *buf, size_t n) {
    /* リセットの直後に、8 ビットのつなぎ方で読む */
    CR = CR_ABORT | CR_STOP;
    DCR = 0x48;
    RCR = 0x20;
    TCR = 0x02;
    IMR = 0;
    ISR = 0xff;
    dma_read(0, buf, (uint16_t)n);
}

static void ring_setup(void) {
    uint8_t i;
    CR = CR_ABORT | CR_STOP;
    DCR = 0x48;
    RBCR0 = 0;
    RBCR1 = 0;
    RCR = 0x20;                    /* 受信は止めておく */
    TCR = 0x02;                    /* 内部ループバック */
    TPSR = RTL_TX_DATA;
    PSTART = RX_START;
    PSTOP = RX_STOP;
    BNRY = RX_START;
    ISR = 0xff;
    IMR = 0;
    CR = CR_ABORT | CR_STOP | CR_PAGE1;
    CURR = RX_START + 1;
    for (i = 0; i < 8; i++) MAR(i) = 0;
    CR = CR_ABORT | CR_STOP;
    rx_next = RX_START + 1;
}

static void set_mac(const uint8_t mac[6]) {
    uint8_t i;
    CR = CR_ABORT | CR_STOP | CR_PAGE1;
    for (i = 0; i < 6; i++) PAR(i) = mac[i];
    CR = CR_ABORT | CR_STOP;
}

void rtl_init(const uint8_t mac[6]) {
    uint8_t id[2];
    rtl_probe(id);
    ring_setup();
    set_mac(mac);
    CR = CR_ABORT | CR_START;
    TCR = 0;
    RCR = RCR_BROADCAST;
}

int rtl_rx_recover(const uint8_t mac[6]) {
    if (!(ISR & ISR_OVW)) return 0;
    ring_setup();
    set_mac(mac);
    CR = CR_ABORT | CR_START;
    TCR = 0;
    RCR = RCR_BROADCAST;
    return 1;
}

static uint8_t read_curr(void) {
    uint8_t c;
    CR = CR_ABORT | CR_START | CR_PAGE1;
    c = CURR;
    CR = CR_ABORT | CR_START;
    return c;
}

static void ring_read(uint16_t addr, uint8_t *buf, uint16_t n) {
    const uint16_t end = (uint16_t)RX_STOP << 8;
    if (addr >= end) addr = (uint16_t)(addr - end + ((uint16_t)RX_START << 8));
    if ((uint32_t)addr + n > end) {
        uint16_t first = (uint16_t)(end - addr);
        dma_read(addr, buf, first);
        dma_read((uint16_t)RX_START << 8, buf + first, (uint16_t)(n - first));
    } else dma_read(addr, buf, n);
}

int rtl_rx_next(rtl_rx *p) {
    uint8_t h[4];
    uint16_t len;
    if (rx_next == read_curr()) return 0;
    dma_read((uint16_t)rx_next << 8, h, 4);
    len = (uint16_t)(h[2] | ((uint16_t)h[3] << 8));
    if (!(h[0] & 1) || h[1] < RX_START || h[1] >= RX_STOP || len < 64 || len > 1522) {
        /* 受信の領域の並びが崩れた: いま書かれているところまで捨てる */
        rx_next = read_curr();
        BNRY = (uint8_t)(rx_next == RX_START ? RX_STOP - 1 : rx_next - 1);
        return 0;
    }
    p->page = rx_next;
    p->next = h[1];
    p->len = (uint16_t)(len - 4);
    return 1;
}

void rtl_rx_read(const rtl_rx *p, uint16_t off, uint8_t *buf, uint16_t n) {
    ring_read((uint16_t)(((uint16_t)p->page << 8) + 4 + off), buf, n);
}

void rtl_rx_done(const rtl_rx *p) {
    rx_next = p->next;
    BNRY = (uint8_t)(p->next == RX_START ? RX_STOP - 1 : p->next - 1);
}

void rtl_tx_write(uint8_t page, uint16_t off, const uint8_t *buf, uint16_t n) {
    if (!n) return;
    dma_setup((uint16_t)(((uint16_t)page << 8) + off), n, CR_WRITE);
    while (n--) RDMAP = *buf++;
    dma_wait();
}

int rtl_tx_go(uint8_t page, uint16_t len) {
    uint16_t i;
    for (i = 0; i < 60000; i++) if (!(CR & CR_TXP)) break;
    if (CR & CR_TXP) return 0;
    if (len < 60) len = 60;
    TPSR = page;
    TBCR0 = (uint8_t)len;
    TBCR1 = (uint8_t)(len >> 8);
    CR = CR_ABORT | CR_START | CR_TXP;
    return 1;
}
