/* nfp_bench.c: wall-clock of soft vs native primitives UNDER QEMU USER MODE. Not cycles: TCG executes every guest integer instruction as a few host
 * instructions but every FP instruction as a softfloat helper call (tens of host instructions), so this UNDERSTATES the native speed-up on real
 * hardware (there fmul/fadd are 3-4 cycle pipelined instructions). It is only a sanity check that native is not slower even here. */
#define S8_NFP_USER 1
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../core/s8gfx_sf.h"
#include "../core/s8gfx_nfp.h"

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static uint32_t rs = 12345;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static f32b in[4096];

#define BENCH(label, expr_soft, expr_nat) do { \
	volatile f32b sink = 0; double t0, t1, t2; long i; \
	t0 = now(); for (i = 0; i < N; i++) { f32b a = in[i & 4095], b = in[(i * 7 + 1) & 4095], c = in[(i * 13 + 5) & 4095]; (void)c; sink += (expr_soft); } \
	t1 = now(); for (i = 0; i < N; i++) { f32b a = in[i & 4095], b = in[(i * 7 + 1) & 4095], c = in[(i * 13 + 5) & 4095]; (void)c; sink += (expr_nat); } \
	t2 = now(); \
	printf("%-10s soft %7.1f ns/op   native %6.1f ns/op   ratio %.1fx\n", label, (t1 - t0) / N * 1e9, (t2 - t1) / N * 1e9, (t1 - t0) / (t2 - t1)); } while (0)

int main(void)
{
	long N = 3000000;
	struct s8_fpsave fs;

	for (int i = 0; i < 4096; i++) in[i] = (rnd() & 0x80000000u) | ((110 + rnd() % 36) << 23) | (rnd() & 0x7fffff);
	s8_fp_begin(&fs);
	BENCH("mul", sf_mul(a, b), nfp_mul(a, b));
	BENCH("add", sf_add(a, b), nfp_add(a, b));
	BENCH("fma", sf_fma(a, b, c), nfp_fma(a, b, c));
	BENCH("div", sf_div(a, b), nfp_div(a, b));
	BENCH("from_i64", sf_from_i64((int32_t)a), nfp_from_i64((int32_t)a));
	BENCH("to_fixed", (f32b)sf_to_fixed(a, 8), (f32b)nfp_to_fixed(a, 8));
	BENCH("to_unorm8", sf_to_unorm8(a), nfp_to_unorm8(a));
	BENCH("f2h", sf_f2h(a), nfp_f2h(a));
	BENCH("h2f", sf_h2f((uint16_t)a), nfp_h2f((uint16_t)a));
	s8_fp_end(&fs);
	return 0;
}
