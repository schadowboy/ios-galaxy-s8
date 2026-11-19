/* host test of s8gfx_sf.h against the FPU: gcc -O0 -ffp-contract=off -o sf_test sf_test.c && ./sf_test */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s8gfx_sf.h"

static float tof(f32b b) { float f; memcpy(&f, &b, 4); return f; }
static f32b tob(float f) { f32b b; memcpy(&b, &f, 4); return b; }

static uint64_t rs = 88172645463325252ULL;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }

/* random float with a spread of exponents, no denormals / inf / nan unless requested */
static f32b rf(int special)
{
	uint32_t s = rnd() & 0x80000000u;
	uint32_t e = 64 + (rnd() % 128);          /* 2^-63 .. 2^64 */
	uint32_t m = rnd() & 0x7fffff;
	if (special && (rnd() % 8) == 0) {
		uint32_t k = rnd() % 6;
		if (k == 0) return s;                                  /* +-0 */
		if (k == 1) return s | 0x3f800000u;                    /* +-1 */
		if (k == 2) return s | 0x3f000000u;                    /* +-0.5 */
		if (k == 3) return s | (rnd() & 0x7f) << 16 | 0x3f000000u;
		if (k == 4) return s | ((rnd() % 3 + 126) << 23);
		return s | ((rnd() % 254 + 1) << 23);                  /* any normal exponent */
	}
	return s | (e << 23) | m;
}

static int flushed(f32b b) { return ((b >> 23) & 0xff) == 0 && (b & 0x7fffff); }

int main(void)
{
	long bad = 0, n = 0;
	long N = 3000000;

	for (long i = 0; i < N; i++) {
		f32b a = rf(1), b = rf(1), c = rf(1);
		float fa = tof(a), fb = tof(b), fc = tof(c);
		f32b hm = tob(fa * fb);
		f32b ha = tob(fa + fb);
		f32b hf = tob(fmaf(fa, fb, fc));
		f32b sm = sf_mul(a, b), sa = sf_add(a, b), sfm = sf_fma(a, b, c);

		/* the host result may be denormal: ours flushes to zero */
		if (flushed(hm)) hm &= 0x80000000u;
		if (flushed(ha)) ha &= 0x80000000u;
		if (flushed(hf)) hf &= 0x80000000u;
		n++;
		if (sm != hm && !(sf_isnan(sm) && sf_isnan(hm))) { if (bad++ < 12) printf("mul %08x * %08x: got %08x want %08x\n", a, b, sm, hm); }
		if (sa != ha && !(sf_isnan(sa) && sf_isnan(ha)) && !((sa | ha) == 0x80000000u && (sa & 0x7fffffffu) == 0 && (ha & 0x7fffffffu) == 0)) {
			if (bad++ < 12) printf("add %08x + %08x: got %08x want %08x\n", a, b, sa, ha);
		}
		if (sfm != hf && !(sf_isnan(sfm) && sf_isnan(hf)) && !((sfm & 0x7fffffffu) == 0 && (hf & 0x7fffffffu) == 0)) {
			if (bad++ < 12) printf("fma %08x * %08x + %08x: got %08x want %08x\n", a, b, c, sfm, hf);
		}
	}
	/* minifloat vs the reference formula */
	for (int v = 0; v < 256; v++) {
		float sign = (v & 0x80) ? -1.0f : 1.0f;
		int exp = (v & 0x70) >> 4;
		unsigned man = v & 0xF;
		float ref = exp ? ldexpf(sign * (float)(man | 0x10), exp - 7) : ldexpf(sign * (float)man, -6);
		f32b g = sf_minifloat((uint8_t)v);
		if (tof(g) != ref && !(ref == 0.0f && tof(g) == 0.0f)) {
			if (bad++ < 20) printf("minifloat %02x: got %g want %g\n", v, tof(g), ref);
		}
	}
	/* unorm8 vs lroundf(x*255) with half up */
	for (int i = 0; i <= 20000; i++) {
		float x = (float)i / 20000.0f;
		uint8_t want = (uint8_t)floorf(x * 255.0f + 0.5f);
		uint8_t got = sf_to_unorm8(tob(x));
		if (got != want && fabsf(x * 255.0f + 0.5f - floorf(x * 255.0f + 0.5f)) > 1e-4f) { if (bad++ < 20) printf("unorm %g: got %d want %d\n", x, got, want); }
	}
	printf("%ld random cases (mul, add, fma each), %ld mismatches\n", n, bad);
	return bad != 0;
}
