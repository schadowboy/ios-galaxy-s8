/*
 * nfp_test.c: native-FP backend (s8gfx_nfp.h) against the integer soft-float (s8gfx_sf.h), bit for bit, on aarch64 (qemu-user or a real core).
 * Build + run: tools/gpu-isa/tests/nfp_test.sh [N]   (aarch64-linux-gnu-gcc -static, run under qemu-aarch64)
 * Exit status 0 = no unexplained mismatch.
 */
#define S8_NFP_USER 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/s8gfx_sf.h"
#include "../core/s8gfx_nfp.h"

static long g_bad;
static uint64_t rs = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd64(void) { rs ^= rs >> 12; rs ^= rs << 25; rs ^= rs >> 27; return rs * 0x2545F4914F6CDD1DULL; }
static uint32_t rnd(void) { return (uint32_t)(rnd64() >> 32); }

static const f32b edge[] = {
	0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x007fffffu, 0x807fffffu, 0x00400000u, 0x00800000u, 0x80800000u, 0x00800001u,
	0x00ffffffu, 0x01000000u, 0x33800000u, 0x34000000u, 0x3f7fffffu, 0x3f800000u, 0xbf800000u, 0x3f800001u, 0x3f000000u, 0x40000000u,
	0x40490fdbu, 0x437f0000u, 0x437f8000u, 0x3b808081u, 0x4b000000u, 0x4b7fffffu, 0x4b800000u, 0x4effffffu, 0x4f000000u, 0xcf000000u,
	0x5f000000u, 0x7e800000u, 0x7f000000u, 0x7f7fffffu, 0xff7fffffu, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u, 0x7f800001u,
	0x7fffffffu, 0x7fc01234u, 0xffa00000u, 0x3dcccccdu, 0x3eaaaaabu, 0x3f333333u, 0x42f6e979u, 0x1a000000u, 0x65000000u,
};
#define NEDGE ((int)(sizeof(edge) / sizeof(edge[0])))

static f32b g_any(void) { return rnd(); }
static f32b g_norm(void) { return (rnd() & 0x80000000u) | ((1 + rnd() % 254) << 23) | (rnd() & 0x7fffff); }
static f32b g_mid(void) { return (rnd() & 0x80000000u) | ((100 + rnd() % 56) << 23) | (rnd() & 0x7fffff); }
static f32b g_den(void) { return (rnd() & 0x80000000u) | (rnd() & 0x7fffff); }
static f32b g_edge(void) { return edge[rnd() % NEDGE]; }
static f32b g_exp(int e, uint32_t sgn) { if (e < 1) e = 1; if (e > 254) e = 254; return sgn | ((uint32_t)e << 23) | (rnd() & 0x7fffff); }

static f32b gen1(int k)
{
	switch (k % 6) {
	case 0: return g_any();
	case 1: return g_norm();
	case 2: return g_mid();
	case 3: return g_edge();
	case 4: return g_den();
	default: return g_mid();
	}
}

struct stat { const char * name; long n, bad, known; };

static int is_nan(f32b a) { return sf_isnan(a); }

/* known difference: soft gave the smallest normal (rounded up out of the denormal range) and the FPU flushed to zero with the same sign */
static int tiny_roundup(f32b soft, f32b nat) { return (soft & 0x7fffffffu) == 0x00800000u && (nat & 0x7fffffffu) == 0 && ((soft ^ nat) & 0x80000000u) == 0; }

#define SHOW 6
static void cmp3(struct stat * s, f32b a, f32b b, f32b c, f32b soft, f32b nat, int ar)
{
	s->n++;
	if (soft == nat) return;
	if (tiny_roundup(soft, nat)) { s->known++; return; }
	if (s->bad++ < SHOW) {
		if (ar == 1) printf("  %s(%08x): soft %08x native %08x\n", s->name, a, soft, nat);
		else if (ar == 2) printf("  %s(%08x, %08x): soft %08x native %08x\n", s->name, a, b, soft, nat);
		else printf("  %s(%08x, %08x, %08x): soft %08x native %08x\n", s->name, a, b, c, soft, nat);
	}
}

static void test_arith(long N)
{
	struct stat sm = { "mul", 0, 0, 0 }, sa = { "add", 0, 0, 0 }, ss = { "sub", 0, 0, 0 }, sd = { "div", 0, 0, 0 }, sf = { "fma", 0, 0, 0 };
	long i;
	int x, y, z;

	/* all pairs / triples of edge values */
	for (x = 0; x < NEDGE; x++) {
		for (y = 0; y < NEDGE; y++) {
			f32b a = edge[x], b = edge[y];

			cmp3(&sm, a, b, 0, sf_mul(a, b), nfp_mul(a, b), 2);
			cmp3(&sa, a, b, 0, sf_add(a, b), nfp_add(a, b), 2);
			cmp3(&ss, a, b, 0, sf_sub(a, b), nfp_sub(a, b), 2);
			cmp3(&sd, a, b, 0, sf_div(a, b), nfp_div(a, b), 2);
			for (z = 0; z < NEDGE; z++) {
				f32b c = edge[z];

				cmp3(&sf, a, b, c, sf_fma(a, b, c), nfp_fma(a, b, c), 3);
			}
		}
	}
	printf("  edge pairs/triples done (%d values)\n", NEDGE);
	for (i = 0; i < N; i++) {
		int k = (int)(i % 36);
		f32b a = gen1(k), b = gen1(k / 6 + (int)(i % 7)), c = gen1((int)(i >> 3));

		switch (i % 8) {
		case 5: {               /* product exponent near the smallest normal (underflow edge) */
			int ea = 1 + rnd() % 254;

			a = g_exp(ea, rnd() & 0x80000000u);
			b = g_exp(128 - ea + (int)(rnd() % 3) - 1, rnd() & 0x80000000u);
			break;
		}
		case 6: {               /* product near overflow */
			int ea = 1 + rnd() % 254;

			a = g_exp(ea, rnd() & 0x80000000u);
			b = g_exp(381 - ea + (int)(rnd() % 3) - 1, rnd() & 0x80000000u);
			break;
		}
		case 3: {               /* product within a few ulps of the smallest normal: b = 2^-126 / a, nudged */
			int ea = 70 + rnd() % 100;

			a = g_exp(ea, rnd() & 0x80000000u);
			b = sf_div(0x00800000u, a & 0x7fffffffu) | (rnd() & 0x80000000u);
			b += (int)(rnd() % 3) - 1;
			break;
		}
		case 7:                 /* cancellation: b ~ -a */
			b = a ^ 0x80000000u;
			b += (int)(rnd() % 5) - 2;
			break;
		}
		cmp3(&sm, a, b, 0, sf_mul(a, b), nfp_mul(a, b), 2);
		cmp3(&sa, a, b, 0, sf_add(a, b), nfp_add(a, b), 2);
		cmp3(&ss, a, b, 0, sf_sub(a, b), nfp_sub(a, b), 2);
		{
			f32b dd = b;

			if (i % 8 == 5) {    /* quotient near the smallest normal: eb = ea + 126 + k */
				int ea = (int)((a >> 23) & 0xff);

				dd = g_exp(ea + 126 + (int)(rnd() % 3) - 1, rnd() & 0x80000000u);
			}
			cmp3(&sd, a, dd, 0, sf_div(a, dd), nfp_div(a, dd), 2);
		}
		if (i % 4 == 0) {       /* fma with c ~ -(a*b) (cancellation) */
			c = sf_mul(a, b) ^ 0x80000000u;
			c += (int)(rnd() % 7) - 3;
		}
		cmp3(&sf, a, b, c, sf_fma(a, b, c), nfp_fma(a, b, c), 3);
	}
	struct stat * all[] = { &sm, &sa, &ss, &sd, &sf };
	for (i = 0; i < 5; i++) {
		printf("%-5s %10ld cases, %ld mismatches, %ld known (tiny result rounded up to 2^-126 by soft, flushed by FPU)\n", all[i]->name, all[i]->n, all[i]->bad, all[i]->known);
	}
	for (i = 0; i < 5; i++) {
		g_bad += all[i]->bad;
	}
}

static void test_conv(long N)
{
	long i, n, bad, nanK, ok;

	/* h2f: exhaustive */
	n = bad = nanK = 0;
	for (uint32_t h = 0; h < 65536; h++) {
		f32b s = sf_h2f((uint16_t)h), f = nfp_h2f((uint16_t)h);

		n++;
		if (s == f) continue;
		if (is_nan(s) && is_nan(f)) { nanK++; continue; }
		if (bad++ < SHOW) printf("  h2f(%04x): soft %08x native %08x\n", h, s, f);
	}
	printf("h2f   %10ld cases (exhaustive), %ld mismatches, %ld NaN payload/quiet differences (both NaN)\n", n, bad, nanK);
	g_bad += bad;

	/* f2h */
	n = bad = nanK = 0;
	for (i = 0; i < N + 65536; i++) {
		f32b a;

		switch (i % 5) {
		case 0: a = g_any(); break;
		case 1: a = g_exp(127 - 26 + (int)(rnd() % 60), rnd() & 0x80000000u); break;               /* half denormal .. overflow range */
		case 2: a = g_exp(127 - 14 + (int)(rnd() % 2) - 1, rnd() & 0x80000000u); break;
		case 3: a = (g_exp(127 - 24 + (int)(rnd() % 40), rnd() & 0x80000000u) & ~0x1fffu) | 0x1000u | (((rnd() % 3) - 1) & 0x3); break;   /* ties and neighbours */
		default: a = i < 65536 ? (uint32_t)i << 16 | (rnd() & 0xffff) : g_edge(); break;
		}
		uint16_t s = sf_f2h(a), f = nfp_f2h(a);

		n++;
		if (s == f) continue;
		if (is_nan(a) && (s & 0x7c00) == 0x7c00 && (s & 0x3ff) && (f & 0x7c00) == 0x7c00 && (f & 0x3ff)) { nanK++; continue; }
		if (bad++ < SHOW) printf("  f2h(%08x): soft %04x native %04x\n", a, s, f);
	}
	printf("f2h   %10ld cases, %ld mismatches, %ld NaN sign differences (both NaN)\n", n, bad, nanK);
	g_bad += bad;

	/* from_i64 */
	n = bad = 0;
	for (i = 0; i < N; i++) {
		int64_t v;

		switch (i % 4) {
		case 0: v = (int64_t)rnd64() >> (rnd() % 64); break;
		case 1: v = (int64_t)(rnd64() >> (rnd() % 64)); break;
		case 2: v = ((int64_t)1 << (rnd() % 63)) + (int64_t)(rnd() % 9) - 4; if (rnd() & 1) v = -v; break;
		default: v = (int64_t)(rnd() % 100000) - 50000; break;
		}
		if (i == 0) v = INT64_MIN;
		if (i == 1) v = INT64_MAX;
		f32b s = sf_from_i64(v), f = nfp_from_i64(v);

		n++;
		if (s != f && bad++ < SHOW) printf("  from_i64(%lld): soft %08x native %08x\n", (long long)v, s, f);
	}
	printf("i64   %10ld cases, %ld mismatches\n", n, bad);
	g_bad += bad;

	/* to_fixed */
	n = bad = 0;
	for (i = 0; i < N; i++) {
		static const int fr[] = { 8, 8, 8, 0, 4, 16, 1 };
		int frac = fr[i % 7];
		f32b a = (i % 3 == 0) ? g_any() : (i % 3 == 1) ? g_exp(127 - 12 + (int)(rnd() % 50), rnd() & 0x80000000u) : g_edge();

		if (i % 11 == 0) a = ((rnd() & 0xff) + (uint32_t)(150 - frac - 8 + (int)(rnd() % 20)) * 0x800000u) ^ (rnd() & 0x80000000u);
		if (i % 13 == 0) a = ((((rnd() % 0x2000) << 11) | 0x400u) & 0x7fffffu) | ((uint32_t)(127 + (int)(rnd() % 12)) << 23);      /* .5 ties */
		int32_t s = sf_to_fixed(a, frac), f = nfp_to_fixed(a, frac);

		n++;
		if (s != f && bad++ < SHOW) printf("  to_fixed(%08x, %d): soft %d native %d\n", a, frac, s, f);
	}
	printf("fixed %10ld cases, %ld mismatches\n", n, bad);
	g_bad += bad;

	/* to_unorm8 */
	n = bad = 0;
	for (i = 0; i < N; i++) {
		f32b a;

		switch (i % 5) {
		case 0: a = g_any(); break;
		case 1: a = g_exp(127 - 12 + (int)(rnd() % 14), 0); break;
		case 2: a = 0x3f800000u - (uint32_t)(rnd() % 0x400000); break;     /* [0.5, 1] */
		case 3: a = g_edge(); break;
		default: a = 0x3b000000u + (rnd() % 0x04800000u); break;      /* 2^-9 .. 1.x */
		}
		uint8_t s = sf_to_unorm8(a), f = nfp_to_unorm8(a);

		n++;
		if (s != f && bad++ < SHOW) printf("  to_unorm8(%08x): soft %u native %u\n", a, s, f);
	}
	/* every k/255 and exact (2k+1)/510 neighbourhoods: walk floats around them */
	for (int k = 0; k <= 255; k++) {
		for (int t = 0; t < 2; t++) {
			double x = t ? (2.0 * k + 1) / 510.0 : (double)k / 255.0;
			float xf = (float)x;
			uint32_t bits;

			memcpy(&bits, &xf, 4);
			for (int d = -3; d <= 3; d++) {
				f32b a = bits + d;
				uint8_t s = sf_to_unorm8(a), f = nfp_to_unorm8(a);

				n++;
				if (s != f && bad++ < SHOW) printf("  to_unorm8(%08x): soft %u native %u\n", a, s, f);
			}
		}
	}
	(void)ok;
	printf("unorm %10ld cases, %ld mismatches\n", n, bad);
	g_bad += bad;
}

/* the FP window: FPCR, FPSR and v0..v7 must come back untouched, FPCR must read FZ|DN inside */
static void get_q(uint8_t * q)
{
	__asm__ volatile (S8NFP_FP
	    "stp q0, q1, [%0, #0]\n\tstp q2, q3, [%0, #32]\n\tstp q4, q5, [%0, #64]\n\tstp q6, q7, [%0, #96]"
	    : : "r" (q) : "memory");
}

static void test_window(void)
{
	uint8_t before[128] __attribute__((aligned(16))), after[128] __attribute__((aligned(16)));
	uint64_t fpcr_in = 0, fpcr_out = 0, fpcr_orig = 0, fpsr_orig = 0, fpcr_after = 0, fpsr_after = 0, user_fpcr = (1ull << 22);   /* a user's RMode = RP */
	struct s8_fpsave fs, fs2;
	long bad = 0;
	int i;

	__asm__ volatile (S8NFP_FP "mrs %0, fpcr\n\tmrs %1, fpsr" : "=r" (fpcr_orig), "=r" (fpsr_orig));
	/* load recognisable values into v0..v7 and a non-default FPCR (user state) */
	__asm__ volatile (S8NFP_FP
	    "msr fpcr, %0\n\t"
	    "movi v0.16b, #0x10\n\tmovi v1.16b, #0x21\n\tmovi v2.16b, #0x32\n\tmovi v3.16b, #0x43\n\t"
	    "movi v4.16b, #0x54\n\tmovi v5.16b, #0x65\n\tmovi v6.16b, #0x76\n\tmovi v7.16b, #0x87"
	    : : "r" (user_fpcr) : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7");
	get_q(before);
	s8_fp_begin(&fs);
	__asm__ volatile (S8NFP_FP "mrs %0, fpcr" : "=r" (fpcr_in));
	if (fpcr_in != S8NFP_FPCR) { bad++; printf("  FPCR inside window %llx, want %llx\n", (unsigned long long)fpcr_in, (unsigned long long)S8NFP_FPCR); }
	s8_fp_begin(&fs2);                      /* nested */
	for (i = 0; i < 100; i++) {
		f32b r = nfp_fma(0x3f800000u + i, 0x40000000u, nfp_div(0x3f800000u, 0x40400000u));
		f32b t = nfp_to_unorm8(r);
		(void)t;
		(void)nfp_h2f(nfp_f2h(r));
	}
	s8_fp_end(&fs2);
	__asm__ volatile (S8NFP_FP "mrs %0, fpcr" : "=r" (fpcr_out));
	if (fpcr_out != S8NFP_FPCR) { bad++; printf("  FPCR after nested end %llx\n", (unsigned long long)fpcr_out); }
	s8_fp_end(&fs);
	get_q(after);
	__asm__ volatile (S8NFP_FP "mrs %0, fpcr\n\tmrs %1, fpsr" : "=r" (fpcr_after), "=r" (fpsr_after));
	if (memcmp(before, after, 128)) { bad++; printf("  v0..v7 not preserved\n"); }
	if (fpcr_after != user_fpcr) { bad++; printf("  FPCR not restored: %llx want %llx\n", (unsigned long long)fpcr_after, (unsigned long long)user_fpcr); }
	__asm__ volatile (S8NFP_FP "msr fpcr, %0\n\tmsr fpsr, %1" : : "r" (fpcr_orig), "r" (fpsr_orig));
	printf("window: FPCR set to FZ|DN inside (%llx), v0..v7/FPCR restored, nested ok: %ld problems\n", (unsigned long long)fpcr_in, bad);
	g_bad += bad;
	{
		int st = s8nfp_selftest();

		printf("s8nfp_selftest() = %d\n", st);
		g_bad += st != 0;
	}
}

int main(int argc, char ** argv)
{
	long N = argc > 1 ? atol(argv[1]) : 2000000;

	printf("nfp_test: N=%ld\n", N);
	test_window();
	{
		struct s8_fpsave fs;

		s8_fp_begin(&fs);       /* FPCR = FZ|DN for the whole comparison, like the kernel window */
		test_arith(N);
		test_conv(N);
		s8_fp_end(&fs);
	}
	printf("TOTAL unexplained mismatches: %ld\n", g_bad);
	return g_bad != 0;
}
