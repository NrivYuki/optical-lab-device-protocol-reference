; 本体の入口(0x1000 番地。更新の仕組みがここへ飛ぶ)。割り込みは使わず、すべて読みに行って待つ。
; スタックを内蔵 RAM の上端近く(0xffff00)に置き、.data をフラッシュから RAM へ写し、.bss を 0 にして main を呼ぶ。
	.h8300h
	.section .start,"ax"
	.global	_start
_start:
	mov.l	#0xffff00,er7
	; .data を写す
	mov.l	#_data_load,er0
	mov.l	#_data_start,er1
	mov.l	#_data_end,er2
1:
	cmp.l	er2,er1
	bhs	2f
	mov.b	@er0+,r3l
	mov.b	r3l,@er1
	adds	#1,er1
	bra	1b
2:
	; .bss を 0 にする
	mov.l	#_bss_start,er0
	mov.l	#_bss_end,er1
	sub.b	r2l,r2l
3:
	cmp.l	er1,er0
	bhs	4f
	mov.b	r2l,@er0
	adds	#1,er0
	bra	3b
4:
	jsr	@_main
5:
	bra	5b
