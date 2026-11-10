; リセットの入口(フラッシュの 0x100)。プログラムを RAM へ写し、.bss を 0 にして main を呼ぶ。
; 本体の update コマンドも、ここへ飛んでくる。
	.h8300h
	.section .vectors,"a"
	.long	_start
	.rept	63
	.long	_start
	.endr

	.section .boot,"ax"
	.global	_start
_start:
	orc	#0x80,ccr		; 割り込みを禁止する
	mov.l	#0xffff00,er7
	mov.l	#_text_load,er0
	mov.l	#_text_start,er1
	mov.l	#_text_end,er2
1:
	cmp.l	er2,er1
	bhs	2f
	mov.b	@er0+,r3l
	mov.b	r3l,@er1
	adds	#1,er1
	bra	1b
2:
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
	mov.l	#_main,er0
	jsr	@er0
5:
	bra	5b

	.section .text
; 内蔵プログラムを呼ぶ: er0・er1 に引数を入れ、er2 の番地へ JSR する。戻り値(FPFR)は r0l(マニュアル 18.4.3)
	.global	_flash_call
_flash_call:
	jsr	@er2
	nop
	rts

; 内蔵プログラムを RAM へ取り出す: FCCS の SCO に 1 を書き、直後に NOP を 4 個置く(18.5.2 (c))
	.global	_flash_sco
_flash_sco:
	mov.l	#0xfee0b0,er0
	bset	#0,@er0
	nop
	nop
	nop
	nop
	rts

; 本体へ飛ぶ
	.global	_jump_app
_jump_app:
	mov.l	#0x1000,er0
	jmp	@er0
