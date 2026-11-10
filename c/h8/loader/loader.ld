/* 更新の仕組み。フラッシュの EB0(0x000000-0x000FFF)に置き、起動すると RAM(0xffd000 から)へ写して動く。
 * フラッシュを消す・書く間はフラッシュから命令を読めないので、全部を RAM で動かす。 */
OUTPUT_FORMAT("elf32-h8300")
OUTPUT_ARCH(h8300h)
ENTRY("_start")
MEMORY
{
	rom(rx)  : o = 0x000000, l = 0x001000
	ram(rwx) : o = 0xffd000, l = 0x002000
}
SECTIONS
{
	.vectors : { KEEP(*(.vectors)) } > rom
	.boot : { KEEP(*(.boot)) } > rom
	.text : {
		_text_start = . ;
		*(.text) *(.text.*) *(.rodata) *(.rodata.*) *(.strings) *(.data) *(.data.*)
		_text_end = . ;
	} > ram AT > rom
	_text_load = LOADADDR(.text);
	.bss : { _bss_start = . ; *(.bss) *(.bss.*) *(COMMON) _bss_end = . ; } > ram
	/DISCARD/ : { *(.comment) *(.note*) }
}
