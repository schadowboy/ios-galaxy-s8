/*
 * Native-FP backend of the AGX stand-in executor (aarch64 only), drop-in for the integer soft-float of s8gfx_sf.h.
 *
 * The XNU kernel is compiled with -mgeneral-regs-only, so the compiler never emits FP/NEON code itself. Every primitive here is one small
 * `asm volatile` block that moves the f32b bit patterns into v0..v3 with fmov, does the operation with the FP unit and moves the result
 * back. `.arch_extension fp` inside the asm string makes the assembler accept the FP instructions in a -mgeneral-regs-only translation unit
 * (clang 21 and gcc 15 both accept it together with the "v0".."v3" clobbers; verified by tools/gpu-isa/tests/nfp/probe.sh).
 *
 * Semantics = s8gfx_sf.h: round to nearest even, fused fma, denormal inputs read as zero and denormal results flushed to zero. That is
 * FPCR.FZ=1; FPCR.DN=1 makes every NaN result the default NaN 0x7fc00000 == SF_QNAN. s8_fp_begin() installs FPCR = FZ|DN (the soft
 * float's behaviour; NOT the IEEE default of FZ=0). Known differences from the soft float (see nfp_test.c):
 *   - sf_h2f / sf_f2h on NaN: soft keeps the payload (h2f) / the sign (f2h); the FPU quiets and canonicalises (sign dropped). Still NaN.
 *   - a result in [2^-126 * (1 - 2^-25), 2^-126) that would round UP to 2^-126: soft returns 2^-126 (tininess after rounding), the FPU
 *     with FZ flushes it to zero (tininess before rounding). Needs an exact result within 1.5e-45 of the smallest normal.
 *   - FPSR flags (inexact, underflow, invalid, ...) accumulate; s8_fp_end restores the caller's FPSR.
 *
 * nfp_* exists for: mul add sub fma div from_i64 to_fixed to_unorm8 h2f f2h. Redirected by default: mul add sub fma div from_i64 (the
 * arithmetic: soft costs 50..250 instructions, native 3..6). to_fixed/to_unorm8/h2f/f2h are redirected only with -DS8NFP_NATIVE_CONV=1
 * (soft is ~30 instructions, native about break-even). The rest of the sf_* API (neg abs isnan isinf sat minifloat) stays integer.
 *
 * Usage: include after s8gfx_sf.h, with S8_NATIVE_FP defined to
 *   1 = runtime switch (global s8_nfp_on, set with s8nfp_enable(); boot-arg s8agxnfp=1): sf_mul etc. dispatch to the native or the soft one
 *   2 = always native (no flag test)
 * The sf_* names are redirected with macros AFTER the soft definitions, so every later user (s8gfx_rast/frag/tex/stream.h, S8Agx.cpp) is
 * switched without editing s8gfx_sf.h. Without S8_NATIVE_FP (or with S8NFP_NO_REMAP) only the nfp_* functions and the fp window API exist.
 *
 * Register safety: XNU spills q0-q31/FPCR/FPSR into the exception frame on EVERY exception entry (SPILL_REGISTERS, osfmk/arm64/
 * exception_asm.h) and reloads them on every return (locore.s Lexception_return_restore_registers), so the live FP registers belong to
 * nobody while the kernel runs. Interrupts and faults inside a window are therefore safe, and the native primitives are correct even
 * outside a window (only the FPCR mode bits could then differ: denormals not flushed, NaN payloads kept). The window s8_fp_begin/end
 * additionally guarantees the mode bits (preemption disabled), makes sure CPACR_EL1.FPEN allows EL1 access and saves/restores v0..v7,
 * FPCR and FPSR (belt and braces: the user's copy already lives in the exception frame).
 */
#ifndef S8GFX_NFP_H
#define S8GFX_NFP_H

#ifndef __aarch64__
#error "s8gfx_nfp.h is aarch64 only"
#endif

#include <stdint.h>
#include "s8gfx_sf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define S8NFP_FP        ".arch_extension fp\n\t"
#define S8NFP_FPCR      ((uint64_t)((1u << 24) | (1u << 25)))      /* FZ | DN, RN, no trap enables, FZ16=0 */

/* ----------------------------------------------------------------- primitives */

static inline f32b
nfp_mul(f32b a, f32b b)
{
	f32b r;

	__asm__ volatile (S8NFP_FP
	    "fmov s0, %w1\n\t"
	    "fmov s1, %w2\n\t"
	    "fmul s0, s0, s1\n\t"
	    "fmov %w0, s0"
	    : "=r" (r) : "r" (a), "r" (b) : "v0", "v1");
	return r;
}

static inline f32b
nfp_add(f32b a, f32b b)
{
	f32b r;

	__asm__ volatile (S8NFP_FP
	    "fmov s0, %w1\n\t"
	    "fmov s1, %w2\n\t"
	    "fadd s0, s0, s1\n\t"
	    "fmov %w0, s0"
	    : "=r" (r) : "r" (a), "r" (b) : "v0", "v1");
	return r;
}

static inline f32b
nfp_sub(f32b a, f32b b)
{
	f32b r;

	__asm__ volatile (S8NFP_FP
	    "fmov s0, %w1\n\t"
	    "fmov s1, %w2\n\t"
	    "fsub s0, s0, s1\n\t"
	    "fmov %w0, s0"
	    : "=r" (r) : "r" (a), "r" (b) : "v0", "v1");
	return r;
}

/* a * b + c, single rounding */
static inline f32b
nfp_fma(f32b a, f32b b, f32b c)
{
	f32b r;

	__asm__ volatile (S8NFP_FP
	    "fmov s0, %w1\n\t"
	    "fmov s1, %w2\n\t"
	    "fmov s2, %w3\n\t"
	    "fmadd s0, s0, s1, s2\n\t"
	    "fmov %w0, s0"
	    : "=r" (r) : "r" (a), "r" (b), "r" (c) : "v0", "v1", "v2");
	return r;
}

static inline f32b
nfp_div(f32b a, f32b b)
{
	f32b r;

	__asm__ volatile (S8NFP_FP
	    "fmov s0, %w1\n\t"
	    "fmov s1, %w2\n\t"
	    "fdiv s0, s0, s1\n\t"
	    "fmov %w0, s0"
	    : "=r" (r) : "r" (a), "r" (b) : "v0", "v1");
	return r;
}

/* int64 -> float, round to nearest even */
static inline f32b
nfp_from_i64(int64_t v)
{
	f32b r;

	__asm__ volatile (S8NFP_FP
	    "scvtf s0, %x1\n\t"
	    "fmov %w0, s0"
	    : "=r" (r) : "r" (v) : "v0");
	return r;
}

/* float -> signed fixed point with frac fractional bits (0 <= frac <= 100), ties away from zero, saturating, NaN -> 0:
 * a * 2^frac is exact (power of two), fcvtas rounds ties away and saturates to int32 */
static inline int32_t
nfp_to_fixed(f32b a, int frac)
{
	int32_t r;
	uint32_t scale = (uint32_t)(127 + frac) << 23;

	__asm__ volatile (S8NFP_FP
	    "fmov s0, %w1\n\t"
	    "fmov s1, %w2\n\t"
	    "fmul s0, s0, s1\n\t"
	    "fcvtas %w0, s0"
	    : "=r" (r) : "r" (a), "r" (scale) : "v0", "v1");
	return r;
}

/* float -> unorm8, round half up (x * 255 + 0.5 floored): x*255 is exact in double, fcvtau rounds ties away (== half up for x >= 0),
 * negatives / NaN give 0, +inf and anything >= 1 saturate to 255 through the clamp */
static inline uint8_t
nfp_to_unorm8(f32b a)
{
	uint32_t r, t;

	__asm__ volatile (S8NFP_FP
	    "fmov s0, %w2\n\t"
	    "fcvt d0, s0\n\t"
	    "mov %w1, #255\n\t"
	    "scvtf d1, %w1\n\t"
	    "fmul d0, d0, d1\n\t"
	    "fcvtau %w0, d0\n\t"
	    "cmp %w0, %w1\n\t"
	    "csel %w0, %w0, %w1, lo"
	    : "=&r" (r), "=&r" (t) : "r" (a) : "v0", "v1", "cc");
	return (uint8_t)r;
}

/* IEEE half -> float (exact); FPCR.FZ16 stays 0 so half denormals convert exactly like the soft version */
static inline f32b
nfp_h2f(uint16_t h)
{
	f32b r;

	__asm__ volatile (S8NFP_FP
	    "fmov s0, %w1\n\t"
	    "fcvt s0, h0\n\t"
	    "fmov %w0, s0"
	    : "=r" (r) : "r" ((uint32_t)h) : "v0");
	return r;
}

/* float -> IEEE half, round to nearest even (half denormal results are kept: FZ16 = 0) */
static inline uint16_t
nfp_f2h(f32b f)
{
	uint32_t r;

	__asm__ volatile (S8NFP_FP
	    "fmov s0, %w1\n\t"
	    "fcvt h0, s0\n\t"
	    "fmov %w0, s0"
	    : "=r" (r) : "r" (f) : "v0");
	return (uint16_t)r;
}

/* ----------------------------------------------------------------- the FP window */

/*
 * s8_fp_begin / s8_fp_end bracket a bounded batch of work (recommended: <= ~1 ms, e.g. 64..256 shaded pixels or one short scanline).
 * Not allowed inside the window: anything that can block or sleep (thread_block, lck_mtx, waiting IOKit calls, page faults on
 * pageable memory), because preemption is disabled. Windows nest (LIFO). Interrupts stay enabled.
 */
struct s8_fpsave {
	uint8_t  q[8 * 16] __attribute__((aligned(16)));        /* v0..v7 of the interrupted context */
	uint64_t fpcr, fpsr, cpacr;
	int      active;                                        /* 0: window was a no-op (native FP switched off) */
	int      cpacr_changed;
};

#ifdef S8_NATIVE_FP
static int s8_nfp_on;                                           /* TU-local run-time switch, set once before any use */
static inline void s8nfp_enable(int on) { s8_nfp_on = on; }
#if S8_NATIVE_FP == 2
#define S8NFP_ON 1
#else
#define S8NFP_ON (s8_nfp_on)
#endif
#else
#define S8NFP_ON 1
#endif

#if defined(KERNEL) && !defined(S8_NFP_USER)
/* osfmk/arm/locks_arm.c: preemption count in the thread; without_measurements skips the SCHED_HYGIENE_DEBUG timing (DEVELOPMENT/DEBUG
 * kernels only), so a long window can never trip "preemption disable timeout exceeded".
 * -DS8NFP_PREEMPT=0 keeps the window (FPCR, CPACR, v0..v7 save) but does not disable preemption: needed if any callback inside the window can
 * sleep (S8Agx resolveVa -> IOMemoryDescriptor::prepare()/map() on first use of a caller-memory range). Without it a preemption can only
 * leave the FPCR mode bits wrong for the rest of the window (denormals not flushed, NaN payloads kept), never corrupt a value: every
 * primitive is a self-contained asm block and XNU spills/reloads the FP registers around every exception. */
#ifndef S8NFP_PREEMPT
#define S8NFP_PREEMPT 1
#endif
#if S8NFP_PREEMPT
extern void _disable_preemption_without_measurements(void);
extern void _enable_preemption(void);
#define S8NFP_PREEMPT_OFF()     _disable_preemption_without_measurements()
#define S8NFP_PREEMPT_ON()      _enable_preemption()
#else
#define S8NFP_PREEMPT_OFF()     ((void)0)
#define S8NFP_PREEMPT_ON()      ((void)0)
#endif
#define S8NFP_CPACR             1
#else
#define S8NFP_PREEMPT_OFF()     ((void)0)
#define S8NFP_PREEMPT_ON()      ((void)0)
#define S8NFP_CPACR             0
#endif

static inline void
s8_fp_begin(struct s8_fpsave * s)
{
	uint64_t cr, sr;

	s->active = 0;
	s->cpacr_changed = 0;
	if (!S8NFP_ON) {
		return;
	}
	S8NFP_PREEMPT_OFF();
#if S8NFP_CPACR
	{
		uint64_t cp;

		__asm__ volatile ("mrs %0, CPACR_EL1" : "=r" (cp));
		s->cpacr = cp;
		if (((cp >> 20) & 3) != 3) {            /* FPEN: 0b11 = no trap at EL0/EL1 (XNU start.s writes this at boot) */
			__asm__ volatile ("msr CPACR_EL1, %0\n\tisb" : : "r" (cp | (3ull << 20)) : "memory");
			s->cpacr_changed = 1;
		}
	}
#endif
	__asm__ volatile (S8NFP_FP
	    "stp q0, q1, [%[q], #0]\n\t"
	    "stp q2, q3, [%[q], #32]\n\t"
	    "stp q4, q5, [%[q], #64]\n\t"
	    "stp q6, q7, [%[q], #96]\n\t"
	    "mrs %[cr], fpcr\n\t"
	    "mrs %[sr], fpsr\n\t"
	    "msr fpcr, %[nw]"
	    : [cr] "=&r" (cr), [sr] "=&r" (sr)
	    : [q] "r" (s->q), [nw] "r" (S8NFP_FPCR)
	    : "memory");
	s->fpcr = cr;
	s->fpsr = sr;
	s->active = 1;
}

static inline void
s8_fp_end(struct s8_fpsave * s)
{
	if (!s->active) {
		return;
	}
	__asm__ volatile (S8NFP_FP
	    "ldp q0, q1, [%[q], #0]\n\t"
	    "ldp q2, q3, [%[q], #32]\n\t"
	    "ldp q4, q5, [%[q], #64]\n\t"
	    "ldp q6, q7, [%[q], #96]\n\t"
	    "msr fpcr, %[cr]\n\t"
	    "msr fpsr, %[sr]"
	    : : [q] "r" (s->q), [cr] "r" (s->fpcr), [sr] "r" (s->fpsr)
	    : "memory", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7");
#if S8NFP_CPACR
	if (s->cpacr_changed) {
		__asm__ volatile ("msr CPACR_EL1, %0\n\tisb" : : "r" (s->cpacr) : "memory");
	}
#endif
	s->active = 0;
	S8NFP_PREEMPT_ON();
}

/* ----------------------------------------------------------------- run-time self test (call once when enabling) */

/* returns 0 when the native primitives agree with the soft ones on a few probes and the window set FZ|DN; nonzero = a failing check number.
 * Needs the soft sf_* (not remapped): uses the *_soft aliases below. Meant for the boot path of s8agxnfp=1. */
#define S8NFP_SOFT(name) name
static inline int
s8nfp_selftest(void)
{
	struct s8_fpsave fs;
	uint64_t cr = 0;
	int bad = 0;
	static const f32b v[] = { 0x3f800000u, 0xc0490fdbu, 0x00800000u, 0x007fffffu, 0x7f7fffffu, 0x7f800000u, 0xff800000u, 0x7fc00001u,
		                  0x80000000u, 0x3eaaaaabu, 0x42f6e979u, 0x00000001u };
	unsigned n = sizeof(v) / sizeof(v[0]), i, j;

	s8_fp_begin(&fs);
	if (fs.active) {
		__asm__ volatile (S8NFP_FP "mrs %0, fpcr" : "=r" (cr));
		if (cr != S8NFP_FPCR) {
			bad |= 1;
		}
	}
	for (i = 0; i < n; i++) {
		for (j = 0; j < n; j++) {
			f32b m = nfp_mul(v[i], v[j]), a = nfp_add(v[i], v[j]), d = nfp_div(v[i], v[j]), f = nfp_fma(v[i], v[j], v[(i + j) % n]);

			if (m != S8NFP_SOFT(sf_mul)(v[i], v[j])) { bad |= 2; }
			if (a != S8NFP_SOFT(sf_add)(v[i], v[j])) { bad |= 4; }
			if (d != S8NFP_SOFT(sf_div)(v[i], v[j])) { bad |= 8; }
			if (f != S8NFP_SOFT(sf_fma)(v[i], v[j], v[(i + j) % n])) { bad |= 16; }
		}
		if (nfp_to_unorm8(v[i]) != S8NFP_SOFT(sf_to_unorm8)(v[i])) { bad |= 32; }
		if (nfp_to_fixed(v[i], 8) != S8NFP_SOFT(sf_to_fixed)(v[i], 8)) { bad |= 64; }
	}
	if (nfp_from_i64(-123456789012345LL) != S8NFP_SOFT(sf_from_i64)(-123456789012345LL)) { bad |= 128; }
	if (nfp_h2f(0x3555) != S8NFP_SOFT(sf_h2f)(0x3555) || nfp_f2h(0x3eaaaaabu) != S8NFP_SOFT(sf_f2h)(0x3eaaaaabu)) { bad |= 256; }
	s8_fp_end(&fs);
	return bad;
}

#ifdef __cplusplus
}
/* scope guard for C++ callers (S8Agx.cpp): `S8FpWindow fpw;` at the top of a function or block */
struct S8FpWindow {
	struct s8_fpsave s;
	S8FpWindow() { s8_fp_begin(&s); }
	~S8FpWindow() { s8_fp_end(&s); }
	void restart() { s8_fp_end(&s); s8_fp_begin(&s); }      /* close and reopen: lets a pending preemption happen between batches */
};
#endif

/* ----------------------------------------------------------------- redirect sf_* (after every soft definition) */

#if defined(S8_NATIVE_FP) && !defined(S8NFP_NO_REMAP)
#define S8NFP_SEL2(nm, ty) \
	static inline ty s8nfp_sel_##nm(f32b a, f32b b) { return S8NFP_ON ? nfp_##nm(a, b) : sf_##nm(a, b); }
S8NFP_SEL2(mul, f32b)
S8NFP_SEL2(add, f32b)
S8NFP_SEL2(sub, f32b)
S8NFP_SEL2(div, f32b)
static inline f32b s8nfp_sel_fma(f32b a, f32b b, f32b c) { return S8NFP_ON ? nfp_fma(a, b, c) : sf_fma(a, b, c); }
static inline f32b s8nfp_sel_from_i64(int64_t v) { return S8NFP_ON ? nfp_from_i64(v) : sf_from_i64(v); }
#define sf_mul          s8nfp_sel_mul
#define sf_add          s8nfp_sel_add
#define sf_sub          s8nfp_sel_sub
#define sf_div          s8nfp_sel_div
#define sf_fma          s8nfp_sel_fma
#define sf_from_i64     s8nfp_sel_from_i64
/*
 * The four conversions are only ~30 integer instructions in the soft float (no 128-bit loop), so the native versions (5..10 instructions
 * but 2..3 GPR<->FP transfers each) are about break-even; under qemu they are even slower (softfloat helper calls). They stay soft unless
 * -DS8NFP_NATIVE_CONV=1 is given: measure on the phone before enabling.
 */
#if defined(S8NFP_NATIVE_CONV) && S8NFP_NATIVE_CONV
static inline int32_t s8nfp_sel_to_fixed(f32b a, int frac) { return S8NFP_ON ? nfp_to_fixed(a, frac) : sf_to_fixed(a, frac); }
static inline uint8_t s8nfp_sel_to_unorm8(f32b a) { return S8NFP_ON ? nfp_to_unorm8(a) : sf_to_unorm8(a); }
static inline f32b s8nfp_sel_h2f(uint16_t h) { return S8NFP_ON ? nfp_h2f(h) : sf_h2f(h); }
static inline uint16_t s8nfp_sel_f2h(f32b f) { return S8NFP_ON ? nfp_f2h(f) : sf_f2h(f); }
#define sf_to_fixed     s8nfp_sel_to_fixed
#define sf_to_unorm8    s8nfp_sel_to_unorm8
#define sf_h2f          s8nfp_sel_h2f
#define sf_f2h          s8nfp_sel_f2h
#endif
#endif

#endif /* S8GFX_NFP_H */
