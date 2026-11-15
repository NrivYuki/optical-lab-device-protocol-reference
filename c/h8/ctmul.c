/* libgcc の 32 ビット・64 ビットの掛け算の置き換え(H8/300H)。
 * libgcc の __mulsi3 は上位 16 ビットが 0 のときに掛け算を省き、__muldi3 は繰り上がりを分岐で足す。どちらも値によって
 * 処理時間が変わり、Poly1305(Monocypher)が秘密の値で呼ぶので、分岐を持たない形で同じ名前の関数を置く
 * (リンカは、ここで決めた名前を libgcc から取り出さない)。16 ビット × 16 ビット → 32 ビットの mulxu.w だけを使う。
 * 組み立てを変えたときは、c/h8/asm/ctmul.s に条件分岐(beq・bne・bls など)が無いことを確かめる。 */
#include <stdint.h>

#define MUL16(a, b) ((uint32_t)(uint16_t)(a) * (uint16_t)(b))

/* 32 ビット × 32 ビットの下位 32 ビット */
uint32_t __mulsi3(uint32_t a, uint32_t b);
uint32_t __mulsi3(uint32_t a, uint32_t b) {
    return MUL16(a, b) + ((MUL16(a >> 16, b) + MUL16(a, b >> 16)) << 16);
}

/* 64 ビット × 64 ビットの下位 64 ビット */
uint64_t __muldi3(uint64_t a, uint64_t b);
uint64_t __muldi3(uint64_t a, uint64_t b) {
    uint32_t a0 = (uint32_t)a, a1 = (uint32_t)(a >> 32), b0 = (uint32_t)b, b1 = (uint32_t)(b >> 32);
    /* a0 × b0 の 64 ビット全体を、16 ビットずつの積で求める。途中の和は 32 ビットに収まるので、繰り上がりの判定は要らない */
    uint32_t p00 = MUL16(a0, b0), p01 = MUL16(a0, b0 >> 16), p10 = MUL16(a0 >> 16, b0), p11 = MUL16(a0 >> 16, b0 >> 16);
    uint32_t mid = (p00 >> 16) + (p01 & 0xffffu) + (p10 & 0xffffu);
    uint32_t lo = (p00 & 0xffffu) | (mid << 16);
    uint32_t hi = p11 + (p01 >> 16) + (p10 >> 16) + (mid >> 16);
    hi += __mulsi3(a0, b1) + __mulsi3(a1, b0);
    return ((uint64_t)hi << 32) | lo;
}
