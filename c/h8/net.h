/* H8 の基板の LAN。ARP・IPv4・ICMP エコー・TCP のクライアント 1 本だけの最小の実装。
 * 送信中のセグメントはコントローラの SRAM に残して再送に使い、H8 の RAM には受信のバッファだけを置く。 */
#ifndef NET_H
#define NET_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t set, pad;              /* set が 1 なら設定済み */
    uint8_t ip[4], mask[4], gw[4], host[4];
    uint16_t port;
} net_cfg;

enum { NET_OK = 0, NET_E_NOLAN = -1, NET_E_ARP = -2, NET_E_CONNECT = -3, NET_E_RESET = -4, NET_E_TIMEOUT = -5 };

/* コントローラを見つけて動かす。無ければ NET_E_NOLAN */
int net_up(const net_cfg *cfg);
int net_is_up(void);
/* 受信を処理する。待っている間いつも呼ぶ */
void net_poll(void);
const uint8_t *net_mac(void);

/* ホストへつなぐ。受信のバッファ(バイト)は呼び出し側が渡す。seed は 6 バイトの乱数(ポートと初期シーケンス番号) */
int net_connect(uint8_t *rx_buf, uint16_t rx_cap, const uint8_t seed[6]);
void net_close(void);
/* 最後に切れたときの理由 */
int net_last_error(void);

/* oldp_io の形 */
int net_write(void *ctx, const uint8_t *buf, size_t n);
int net_flush(void *ctx);
int net_read(void *ctx, uint8_t *buf, size_t n, uint32_t timeout_ms);

typedef struct { uint32_t rx, tx, retrans, bad_csum, overflow; } net_stats_t;
extern net_stats_t net_stats;

#endif
