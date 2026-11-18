/* s8-qemu boot stage: the QEMU counterpart of shim/dreamlte/loader.c.
 * Same hand-over (SWEETXNU payload -> kernel segments at phys_base, ADT, boot_args, branch to the entry with x0 = boot_args PA,
 * MMU off, EL1) but without a single hardware poke (no UFS/PMIC/ACPM/display/ramoops). Only the optional debug UART is touched.
 * Differences from the phone loader: RAM is not zeroed (QEMU RAM starts zero), no s8rtc fixup (make-payload.sh bakes s8rtc in),
 * no s8bigboot / Mongoose hand-off.
 * Build-time defines: PAYLOAD_PA (SWEETXNU payload location), UART_BASE (PL011, 0 = silent),
 * XNU_RAM_BASE/XNU_RAM_END (bank the payload's phys_base must lie in; default 0x880000000 bank, as on the phone). */
#include <stdint.h>

#ifndef PAYLOAD_PA
#error PAYLOAD_PA
#endif
#ifndef UART_BASE
#define UART_BASE 0
#endif
#ifndef XNU_RAM_BASE
#define XNU_RAM_BASE 0x880000000UL
#define XNU_RAM_END  0x900000000UL
#endif
#define PAGE_SIZE 4096UL
#define BOOT_LINE_LENGTH 608

struct payload_seg { uint64_t vmaddr, vmsize, file_off, file_size; };
struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};
struct boot_video { uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth; };
struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};
_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args size");

uint64_t qb_boot_args;	/* read by start.S */

static void out(const char *s)
{
	if (!UART_BASE)
		return;
	while (*s) {
		if (*s == '\n')
			*(volatile uint32_t *)UART_BASE = '\r';
		*(volatile uint32_t *)UART_BASE = (unsigned char)*s++;
	}
}

static void hex(uint64_t v)
{
	static const char d[] = "0123456789abcdef";
	char b[19] = "0x";
	int n = 2, started = 0;
	for (int i = 60; i >= 0; i -= 4) {
		int x = (v >> i) & 0xf;
		if (x || started || i == 0) {
			b[n++] = d[x];
			started = 1;
		}
	}
	b[n] = 0;
	out(b);
}

static void kv(const char *k, uint64_t v)
{
	out(k);
	out("=");
	hex(v);
	out(" ");
}

static void copy(uintptr_t dst, uintptr_t src, uint64_t n)
{
	if (!((dst | src) & 7))
		for (; n >= 8; n -= 8, dst += 8, src += 8)
			*(volatile uint64_t *)dst = *(volatile uint64_t *)src;
	for (; n; n--, dst++, src++)
		*(volatile uint8_t *)dst = *(volatile uint8_t *)src;
}

static void zero(uintptr_t dst, uint64_t n)
{
	for (; n && (dst & 7); n--, dst++)
		*(volatile uint8_t *)dst = 0;
	for (; n >= 8; n -= 8, dst += 8)
		*(volatile uint64_t *)dst = 0;
	for (; n; n--, dst++)
		*(volatile uint8_t *)dst = 0;
}

static void clean(uintptr_t start, uintptr_t end)
{
	uint64_t ctr;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	uintptr_t line = 4UL << ((ctr >> 16) & 0xf);
	for (uintptr_t p = start & ~(line - 1); p < end; p += line)
		__asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t fail(const char *why)
{
	out("FAIL: ");
	out(why);
	out("\n");
	return 0;
}

uint64_t qb_main(uint64_t el)
{
	out("S8QEMU LOADER BEGIN\n");
	kv("EL", el);
	out("\n");
	const volatile struct payload *p = (const volatile struct payload *)PAYLOAD_PA;
	const char *want = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (p->magic[i] != want[i])
			return fail("no SWEETXNU payload");
	if (p->version != 1)
		return fail("payload version");
	uint64_t nsegs = p->nsegs;
	if (!nsegs || nsegs > 32)
		return fail("segs");
	if (p->phys_base < XNU_RAM_BASE || p->phys_base >= XNU_RAM_END || !p->mem_size ||
	    p->mem_size > XNU_RAM_END - p->phys_base || p->entry_va < p->virt_base || p->kernel_end < p->virt_base)
		return fail("payload RAM outside the XNU bank");
	kv("phys", p->phys_base);
	kv("mem", p->mem_size);
	kv("adt", p->adt_size);
	out("\n");
	uintptr_t mem_end = p->phys_base + p->mem_size;
	uintptr_t kernel_end = p->phys_base + (p->kernel_end - p->virt_base);
	if (kernel_end >= mem_end)
		return fail("kernel larger than RAM window");
	for (uint64_t i = 0; i < nsegs; i++) {
		const volatile struct payload_seg *s = &p->segs[i];
		if (s->vmaddr < p->virt_base || s->file_size > s->vmsize ||
		    s->vmsize > p->mem_size || s->vmaddr - p->virt_base > p->mem_size - s->vmsize)
			return fail("bad segment");
		uintptr_t dst = p->phys_base + (s->vmaddr - p->virt_base);
		copy(dst, PAYLOAD_PA + s->file_off, s->file_size);
		zero(dst + s->file_size, s->vmsize - s->file_size);
		clean(dst, dst + s->vmsize);
		out("seg ");
		hex(i);
		out(" -> ");
		hex(dst);
		out(" ");
		hex(s->vmsize);
		out("\n");
	}
	uintptr_t adt_pa = kernel_end;
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	if (ba_pa + PAGE_SIZE > mem_end)
		return fail("no room for ADT/boot_args");
	copy(adt_pa, PAYLOAD_PA + p->adt_off, p->adt_size);
	zero(ba_pa, PAGE_SIZE);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = ba_pa + PAGE_SIZE;
	ba->deviceTreeP = p->virt_base + (adt_pa - p->phys_base);
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	clean(adt_pa, ba_pa + PAGE_SIZE);
	uintptr_t entry_pa = p->phys_base + (p->entry_va - p->virt_base);
	if (entry_pa >= kernel_end)
		return fail("entry outside kernel");
	qb_boot_args = ba_pa;
	kv("adt_pa", adt_pa);
	kv("boot_args", ba_pa);
	kv("entry_pa", entry_pa);
	out("\nJUMP TO XNU\n");
	return entry_pa;
}

void *memset(void *d, int c, unsigned long n)
{
	unsigned char *q = d;
	while (n--)
		*q++ = (unsigned char)c;
	return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
	unsigned char *q = d;
	const unsigned char *r = s;
	while (n--)
		*q++ = *r++;
	return d;
}
