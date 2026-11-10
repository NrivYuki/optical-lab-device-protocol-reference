/* 本体。内蔵フラッシュの 0x001000 番地から(0x000000-0x000FFF の EB0 には更新の仕組みがいる)にプログラムと定数を置き、
 * 内蔵 RAM(0xffbf20 から 16 KB)に変数とスタックを置く。 */
OUTPUT_FORMAT("elf32-h8300")
OUTPUT_ARCH(h8300h)
ENTRY("_start")

MEMORY
{
	rom(rx)  : o = 0x001000, l = 0x07f000
	ram(rwx) : o = 0xffbf20, l = 0x003f00
}

SECTIONS
{
	.text : {
		KEEP(*(.start))
		*(.text)
		*(.text.*)
		*(.rodata)
		*(.rodata.*)
		*(.strings)
	} > rom
	.data : {
		_data_start = . ;
		*(.data)
		*(.data.*)
		. = ALIGN(2);		/* 命令は偶数番地に置く */
		*(.ramtext)
		_data_end = . ;
	} > ram AT > rom
	_data_load = LOADADDR(.data);
	.bss : {
		_bss_start = . ;
		*(.bss)
		*(.bss.*)
		*(COMMON)
		_bss_end = . ;
	} > ram
	ASSERT(_bss_end <= 0xffdf20, "変数が内蔵プログラムの取り出し先(0xffdf20)に重なっています")
	/DISCARD/ : { *(.comment) *(.note*) }
}
