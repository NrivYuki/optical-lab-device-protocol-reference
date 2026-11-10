; 内蔵プログラムの呼び出し(store.c から。RAM に写して使う)
	.h8300h
	.section .ramtext,"ax"
; er0・er1 に引数を入れ、er2 の番地へ JSR する。戻り値(FPFR)は r0l
	.global	_store_call
_store_call:
	jsr	@er2
	nop
	rts

; 内蔵プログラムを RAM へ取り出す: FCCS の SCO に 1 を書き、直後に NOP を 4 個置く
	.global	_store_sco
_store_sco:
	mov.l	#0xfee0b0,er0
	bset	#0,@er0
	nop
	nop
	nop
	nop
	rts
