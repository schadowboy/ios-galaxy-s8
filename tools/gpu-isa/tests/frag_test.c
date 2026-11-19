/* host test: gcc -O0 -ffp-contract=off -o frag_test frag_test.c -lm && ./frag_test scenes.bin */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s8gfx_frag.h"

static float tof(f32b b) { float f; memcpy(&f, &b, 4); return f; }
static f32b tob(float f) { f32b b; memcpy(&b, &f, 4); return b; }

typedef void (*ref_fn)(const float * c, float * o);
static void r10(const float * c, float * o) { for (int i = 0; i < 4; i++) o[i] = c[i] * 0.5f; }
static void r11(const float * c, float * o) { for (int i = 0; i < 4; i++) o[i] = c[i] + 0.125f; }
static void r12(const float * c, float * o) { for (int i = 0; i < 4; i++) o[i] = c[i] * c[i]; }
static void r13(const float * c, float * o) { for (int i = 0; i < 4; i++) o[i] = fmaf(c[i], 0.5f, 0.25f); }
static void r15(const float * c, float * o) { for (int i = 0; i < 4; i++) { float v = fmaf(c[i], 2.0f, -0.5f); o[i] = v < 0 ? 0 : (v > 1 ? 1 : v); } }
static void r16(const float * c, float * o) { o[0] = 1.0f - c[0]; o[1] = 0; o[2] = c[0]; o[3] = 1.0f; }
static void r22(const float * c, float * o) { for (int i = 0; i < 4; i++) o[i] = c[i] * 0.5f; }
static void r3(const float * c, float * o) { (void)c; o[0] = 1; o[1] = 0; o[2] = 0; o[3] = 1; }
static void r4(const float * c, float * o) { (void)c; o[0] = 0; o[1] = 1; o[2] = 0; o[3] = 1; }
static void r5(const float * c, float * o) { (void)c; o[0] = 0; o[1] = 0; o[2] = 1; o[3] = 1; }
static void r6(const float * c, float * o) { (void)c; o[0] = 0.5f; o[1] = 0.25f; o[2] = 0.125f; o[3] = 1; }
static void r7(const float * c, float * o) { for (int i = 0; i < 4; i++) o[i] = c[i]; }

int main(int argc, char ** argv)
{
	FILE * f = fopen(argv[1], "rb");
	struct { int id; ref_fn fn; } refs[] = { { 3, r3 }, { 4, r4 }, { 5, r5 }, { 6, r6 }, { 7, r7 }, { 10, r10 }, { 11, r11 }, { 12, r12 }, { 13, r13 }, { 15, r15 }, { 16, r16 }, { 22, r22 } };
	static uint8_t prog[35][512];
	uint32_t len[36] = { 0 };
	uint32_t id, l;
	int bad = 0;

	while (fread(&id, 4, 1, f) == 1 && fread(&l, 4, 1, f) == 1) {
		fread(prog[id - 1], 1, l < 512 ? l : 512, f);
		len[id] = l;
	}
	for (unsigned k = 0; k < sizeof(refs) / sizeof(refs[0]); k++) {
		int s = refs[k].id;
		int sbad = 0;

		for (int py = 0; py < 64 && !sbad; py += 5) {
			for (int px = 0; px < 64; px += 7) {
				float c[4] = { (px + 0.5f) / 64.0f, 1.0f - (py + 0.5f) / 64.0f, 0.25f, 1.0f };
				f32b vv[4] = { tob(c[0]), tob(c[1]), tob(c[2]), tob(c[3]) };
				struct s8f_ctx ctx;
				float want[4];

				memset(&ctx, 0, sizeof(ctx));
				ctx.varying = vv;
				int ok = s8f_run(&ctx, prog[s - 1], len[s]);
				refs[k].fn(c, want);
				if (!ok) { printf("scene %d: program did not complete (unknown opcode 0x%02x)\n", s, ctx.unknown); sbad = 1; break; }
				for (int i = 0; i < 4; i++) {
					if (fabsf(tof(ctx.out[i]) - want[i]) > 1e-6f) { printf("scene %d pixel (%d,%d) ch %d: got %g want %g\n", s, px, py, i, tof(ctx.out[i]), want[i]); sbad = 1; break; }
				}
				if (sbad) break;
			}
		}
		printf("scene %2d: %s\n", s, sbad ? "MISMATCH" : "interpreter output equals the host FPU reference");
		bad += sbad;
	}
	return bad != 0;
}
