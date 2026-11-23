#include <stdint.h>
static inline uint32_t fadd(uint32_t a, uint32_t b)
{
	uint32_t r;
	__asm__ volatile(
		".arch_extension fp\n\t"
		"fmov s0, %w1\n\t"
		"fmov s1, %w2\n\t"
		"fadd s0, s0, s1\n\t"
		"fmov %w0, s0\n\t"
		: "=r"(r) : "r"(a), "r"(b) : "v0", "v1");
	return r;
}
uint32_t t(uint32_t a, uint32_t b) { return fadd(a, fadd(b, a)); }
