/* H8/3069F の内蔵レジスタ(H8/3069 F-ZTAT ハードウェアマニュアル)。番地はアドバンストモードの 24 ビット。 */
#ifndef OLDP_H8_H
#define OLDP_H8_H

#define REG8(a)  (*(volatile unsigned char *)(a))
#define REG16(a) (*(volatile unsigned short *)(a))

/* SCI1(13 章) */
#define SCI1_SMR REG8(0xffffb8)
#define SCI1_BRR REG8(0xffffb9)
#define SCI1_SCR REG8(0xffffba)
#define SCI1_TDR REG8(0xffffbb)
#define SCI1_SSR REG8(0xffffbc)
#define SCI1_RDR REG8(0xffffbd)
#define SCR_TE   0x20
#define SCR_RE   0x10
#define SSR_TDRE 0x80
#define SSR_RDRF 0x40
#define SSR_ORER 0x20
#define SSR_FER  0x10
#define SSR_PER  0x08

/* ポートとバス(7 章・6 章) */
#define P1DDR    REG8(0xfee000)
#define P8DDR    REG8(0xfee007)
#define MDCR     REG8(0xfee011)

/* 16 ビットタイマ チャネル 0(9 章) */
#define TSTR     REG8(0xffff60)
#define TCR0     REG8(0xffff68)
#define TCNT0    REG16(0xffff6a)

#endif
