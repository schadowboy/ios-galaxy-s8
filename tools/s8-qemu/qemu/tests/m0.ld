ENTRY(_start)
SECTIONS
{
	. = 0x80080000;
	.text : { *(.text.head) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) }
}
