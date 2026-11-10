/* RTL8019AS(NE2000 互換の 10BASE-T コントローラ)のドライバ。外部バスのエリア 1(0x200000)に 8 ビットでつながる。
 * 割り込みは使わず、呼び出し側が受信を見に来る。パケットのバッファはコントローラの中の SRAM を使い、H8 の RAM を使わない。 */
#ifndef RTL8019_H
#define RTL8019_H
#include <stdint.h>
#include <stddef.h>

/* 送信のページ。データは 1 つの TCP セグメントを、再送に備えて次の送信まで SRAM に残す */
#define RTL_TX_DATA 0x40
#define RTL_TX_CTRL 0x46

typedef struct {
    uint16_t len;                  /* イーサネットのヘッダから、CRC を除いた長さ */
    uint8_t page, next;
} rtl_rx;

void rtl_bus_init(void);
/* リセットして ID を読む。id[0]='P'、id[1]='p' なら RTL8019。リセットの完了を確かめられたら 1 */
int rtl_probe(uint8_t id[2]);
void rtl_read_prom(uint8_t *buf, size_t n);
void rtl_init(const uint8_t mac[6]);

/* 受信: 1 パケットあれば 1。読み終えたら rtl_rx_done */
int rtl_rx_next(rtl_rx *p);
void rtl_rx_read(const rtl_rx *p, uint16_t off, uint8_t *buf, uint16_t n);
void rtl_rx_done(const rtl_rx *p);
/* 受信の領域があふれたらコントローラをやり直す(失ったパケットは TCP の再送で補う)。やり直したら 1 */
int rtl_rx_recover(const uint8_t mac[6]);

void rtl_tx_write(uint8_t page, uint16_t off, const uint8_t *buf, uint16_t n);
/* 書いておいたパケットを送る。前の送信が終わらなければ 0 */
int rtl_tx_go(uint8_t page, uint16_t len);

#endif
