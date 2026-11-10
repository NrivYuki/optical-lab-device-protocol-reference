/* 覚えたホストをフラッシュに残す。FWE 端子が 1 のときだけ書ける */
#ifndef OLDP_STORE_H
#define OLDP_STORE_H
#include <stdint.h>

/* block を消し、addr に 128 バイト(page は偶数番地の RAM)を書く。0 が成功 */
uint8_t store_page(const uint8_t *page, uint32_t addr, uint8_t block);

#endif
