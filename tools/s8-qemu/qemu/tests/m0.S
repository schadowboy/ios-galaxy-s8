/* M0 bare-metal test for the s8-a5xelte QEMU machine. Loaded at 0x80080000.
 * GIC + arch timer IRQ + PL011 + RAZ/WI MMIO reads + PMU stub + PSCI CPU_ON/SYSTEM_OFF. */
#define UART   0x13800000
#define GICD   0x10201000
#define GICC   0x10202000
#define PMU    0x16480000

	.section .text.head, "ax"
	.globl _start
_start:
	ldr	x2, =stack0_top
	mov	sp, x2
	adr	x2, vectors
	msr	vbar_el1, x2
	isb
	adr	x0, s_boot
	bl	puts
	mrs	x0, mpidr_el1
	bl	puthex
	adr	x0, s_cnt
	bl	puts
	mrs	x0, cntfrq_el0
	bl	puthex
	adr	x0, s_el
	bl	puts
	mrs	x0, CurrentEL
	lsr	x0, x0, #2
	bl	puthex

	/* RAZ/WI probes */
	adr	x0, s_raz
	bl	puts
	ldr	x1, =0x10430000
	ldr	w0, [x1]
	bl	puthex
	ldr	x1, =0x11120000
	ldr	w0, [x1]
	bl	puthex
	ldr	x1, =0x1fff0000
	mov	w0, #0x1234
	str	w0, [x1]
	ldr	w0, [x1]
	bl	puthex

	/* PMU stub: CPU1_CONFIGURATION (+0x2080) = 0xf -> STATUS (+0x2084) mirrors */
	adr	x0, s_pmu
	bl	puts
	ldr	x1, =PMU
	mov	x2, #0x2080
	mov	w3, #0xf
	str	w3, [x1, x2]
	add	x2, x2, #4
	ldr	w0, [x1, x2]
	bl	puthex

	/* GIC + timer */
	ldr	x1, =GICD
	mov	w0, #1
	str	w0, [x1]
	ldr	x2, =GICC
	mov	w0, #0xff
	str	w0, [x2, #4]
	mov	w0, #1
	str	w0, [x2]
	mov	w0, #(1 << 30)
	str	w0, [x1, #0x100]	/* ISENABLER0: PPI 14 = INTID 30 */
	mov	x0, #26000
	msr	cntp_tval_el0, x0
	mov	x0, #1
	msr	cntp_ctl_el0, x0
	msr	daifclr, #2
2:	wfi
	ldr	x1, =irq_flag
	ldr	w0, [x1]
	cbz	w0, 2b
	adr	x0, s_irqok
	bl	puts

	/* PSCI CPU_ON cpu1 (MPIDR 1) */
	ldr	x0, =0xc4000003
	mov	x1, #1
	adr	x2, secondary
	mov	x3, #0
	smc	#0
	bl	puthex			/* PSCI return code (0 = success) */
3:	ldr	x1, =cpu1_flag
	ldr	w0, [x1]
	cbz	w0, 3b
	adr	x0, s_done
	bl	puts
	ldr	x0, =0x84000008		/* PSCI SYSTEM_OFF */
	smc	#0
4:	wfi
	b	4b
1:

secondary:
	ldr	x2, =stack1_top
	mov	sp, x2
	adr	x0, s_cpu1
	bl	puts
	mrs	x0, mpidr_el1
	bl	puthex
	ldr	x1, =cpu1_flag
	mov	w0, #1
	str	w0, [x1]
	dsb	sy
5:	wfe
	b	5b

irq_handler:
	ldr	x9, =GICC
	ldr	w10, [x9, #0xc]		/* IAR */
	mov	x0, #0
	msr	cntp_ctl_el0, x0	/* stop the timer */
	adr	x0, s_irq
	stp	x30, x10, [sp, #-16]!
	bl	puts
	ldr	x10, [sp, #8]
	and	x0, x10, #0x3ff
	bl	puthex
	ldp	x30, x10, [sp], #16
	ldr	x9, =GICC
	str	w10, [x9, #0x10]	/* EOIR */
	ldr	x9, =irq_flag
	mov	w0, #1
	str	w0, [x9]
	eret

/* puts(x0 = string) */
puts:
	ldr	x9, =UART
1:	ldrb	w10, [x0], #1
	cbz	w10, 3f
2:	ldr	w11, [x9, #0x18]
	tbnz	w11, #5, 2b
	str	w10, [x9]
	b	1b
3:	ret

/* puthex(x0 = value): " 0x%016x\n" */
puthex:
	ldr	x9, =UART
	mov	w10, #' '
	str	w10, [x9]
	mov	w10, #'0'
	str	w10, [x9]
	mov	w10, #'x'
	str	w10, [x9]
	mov	x12, #60
1:	lsr	x13, x0, x12
	and	x13, x13, #0xf
	cmp	x13, #10
	b.lt	2f
	add	x13, x13, #('a' - '0' - 10)
2:	add	x13, x13, #'0'
3:	ldr	w11, [x9, #0x18]
	tbnz	w11, #5, 3b
	str	w13, [x9]
	subs	x12, x12, #4
	b.ge	1b
	mov	w10, #'\n'
	str	w10, [x9]
	ret

	.balign 0x800
vectors:
	.rept 4
	.balign 0x80
	b	.
	.endr
	/* current EL, SPx: sync 0x200, irq 0x280 */
	.balign 0x80
	b	.
	.balign 0x80
	b	irq_handler
	.balign 0x80
	b	.
	.balign 0x80
	b	.
	.rept 8
	.balign 0x80
	b	.
	.endr

	.pool
s_boot:	.asciz "M0 s8-a5xelte boot cpu MPIDR"
s_cnt:	.asciz "CNTFRQ"
s_el:	.asciz "EL"
s_raz:	.asciz "RAZ/WI probes (uart0 dts, ufs hci, wi window):"
s_pmu:	.asciz "PMU CPU1_STATUS after CONFIGURATION=0xf:"
s_irq:	.asciz "timer IRQ taken, INTID"
s_irqok:.asciz "irq flag seen\n"
s_cpu1:	.asciz "hello from secondary cpu, MPIDR"
s_done:	.asciz "M0 DONE\n"

	.section .bss
	.balign 16
irq_flag:  .skip 8
cpu1_flag: .skip 8
	.skip 4096
stack0_top:
	.skip 4096
stack1_top:
