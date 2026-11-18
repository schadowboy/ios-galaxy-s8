/* s8-qemu boot stage entry. QEMU starts us at EL1 (or EL2 if the machine enables virtualization), MMU off. */
	.section .text.head, "ax"
	.globl _start
_start:
	msr	daifset, #0xf
	mrs	x1, CurrentEL
	cmp	x1, #0x8
	b.ne	1f
	/* EL2 -> EL1, same recipe as shim/dreamlte mongoose_boot_entry */
	mov	x1, #(1 << 31)
	msr	hcr_el2, x1
	msr	vttbr_el2, xzr
	msr	cptr_el2, xzr
	msr	mdcr_el2, xzr
	mov	x1, #3
	msr	cnthctl_el2, x1
	msr	cntvoff_el2, xzr
	movz	x1, #0x0800
	movk	x1, #0x30d0, lsl #16
	msr	sctlr_el1, x1
	isb
	adr	x1, 1f
	msr	elr_el2, x1
	mov	x1, #0x3c5
	msr	spsr_el2, x1
	eret
1:	msr	spsel, #1
	adr	x1, _stack_top
	mov	sp, x1
	adr	x1, _bss_start
	adr	x2, _bss_end
2:	cmp	x1, x2
	b.hs	3f
	str	xzr, [x1], #8
	b	2b
3:	mrs	x0, CurrentEL
	lsr	x0, x0, #2
	bl	qb_main			/* returns kernel entry PA, or 0 */
	cbz	x0, 5f
	mov	x16, x0
	adr	x1, qb_boot_args
	ldr	x0, [x1]		/* boot_args PA, as the phone loader hands over */
	mrs	x14, sctlr_el1
	bic	x14, x14, #1
	bic	x14, x14, #4
	bic	x14, x14, #0x1000
	msr	sctlr_el1, x14
	ic	iallu
	dsb	sy
	isb
	br	x16
5:	wfe
	b	5b
