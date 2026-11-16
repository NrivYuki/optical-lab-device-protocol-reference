/* フラッシュの 1 ブロックを書き換える(覚えたホスト・機器の鍵・更新の仕組み)。FWE 端子が 1 のときだけ書ける */
#ifndef OLDP_STORE_H
#define OLDP_STORE_H
#include <stdint.h>

/* block を消し、addr に 128 バイト(page は偶数番地の RAM)を書く。0 が成功 */
uint8_t store_page(const uint8_t *page, uint32_t addr, uint8_t block);
/* block を消し、addr から 128 バイトずつ pages 個(data は偶数番地の RAM)を書く。0 が成功 */
uint8_t store_block(const uint8_t *data, uint32_t addr, uint8_t block, uint16_t pages);
/* 消さずに、addr に 128 バイト(data は偶数番地の RAM)を書く。addr のページは消えたまま(全て 0xff)であること。0 が成功 */
uint8_t store_program(const uint8_t *data, uint32_t addr);

#endif
