/*
 * Host test of the A11 forms added for the compositor corpus (analysis/a11-isa-forms.md). gcc -O0 -ffp-contract=off -I../core -o isa_test isa_test.c -lm
 * usage: isa_test [corpus4.bin]
 *
 * Programs g3, g5, g7 are the captured bytes of my own round-8 test shaders (userland/s8mali/s8mtl_test.m, capture15), scenes 31 and 35 are captured from the
 * 35-scene family (capture9); the expected values below are computed with host float/integer C arithmetic from the shader SOURCE, not from the decoded semantics.
 * The last test runs the compositor's frag 5 (an MSAA-resolve shaped program) on synthetic samples: its f16 dataflow is ASSUMED and only checked for consistency.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s8gfx_frag.h"

static float tof(f32b b) { float f; memcpy(&f, &b, 4); return f; }
static f32b tob(float f) { f32b b; memcpy(&b, &f, 4); return b; }
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
static float rndf(void) { return (rnd() & 0xffff) / 65535.0f; }

/* pack_float_to_unorm4x8 reference: round to nearest (ties away; the inputs below never sit on a tie) */
static uint8_t q8(float x) { double d = x; if (d < 0) d = 0; if (d > 1) d = 1; return (uint8_t)floor(d * 255.0 + 0.5); }

/* g3: uint p = pack(col); uint q = (p >> 1) & 0x7f7f7f7f; return unpack(q); (session 7 of capture15) */
static const uint8_t g3[] = {
	0x51, 0x02, 0x01, 0x80, 0x00, 0x00, 0x10, 0x00, 0x45, 0x02, 0x00, 0x11, 0x01, 0xc0, 0xf2, 0x00, 0x05, 0x01, 0x40, 0x11, 0x00, 0xc0, 0x12, 0x00, 0x38, 0x00,
	0x6e, 0x02, 0x00, 0xc0, 0x05, 0x80, 0x01, 0x00, 0xd2, 0x02, 0x7f, 0x7f, 0x7f, 0x7f, 0x7e, 0x00, 0xc0, 0xc2, 0x87, 0x00,
	0x45, 0x01, 0x00, 0x11, 0x00, 0xc0, 0x12, 0x00, 0x05, 0x02, 0x00, 0x11, 0x00, 0xc0, 0xf2, 0x00, 0x38, 0x00,
	0x48, 0x00, 0xc2, 0x00, 0x48, 0x0c, 0x00, 0x00, 0x09, 0x02, 0x00, 0x00, 0x00, 0x00, 0xf2, 0xff, 0x08, 0x01, 0x00, 0x00 };
/* g5: p ^ 0xff00ff00 (round8b log line 7585) */
static const uint8_t g5[] = {
	0x51, 0x02, 0x01, 0x80, 0x00, 0x00, 0x10, 0x00, 0x45, 0x02, 0x00, 0x11, 0x01, 0xc0, 0xf2, 0x00, 0x05, 0x01, 0x40, 0x11, 0x00, 0xc0, 0x12, 0x00, 0x38, 0x00,
	0xd2, 0x02, 0x00, 0xff, 0x00, 0xff, 0x7e, 0x00, 0xc0, 0xc2, 0x67, 0x00,
	0x45, 0x01, 0x00, 0x11, 0x00, 0xc0, 0x12, 0x00, 0x05, 0x02, 0x00, 0x11, 0x00, 0xc0, 0xf2, 0x00, 0x38, 0x00,
	0x48, 0x00, 0xc2, 0x00, 0x48, 0x0c, 0x00, 0x00, 0x09, 0x02, 0x00, 0x00, 0x00, 0x00, 0xf2, 0xff, 0x08, 0x01, 0x00, 0x00 };
/* g7: p = pack(dst [[color(0)]]); q = (p & 0x00ff00ff) | 0x80000000; return unpack(q) * col (session 6 of capture15) */
static const uint8_t g7[] = {
	0x48, 0x00, 0xc2, 0x00, 0x48, 0x08, 0x00, 0x00, 0x79, 0x02, 0x00, 0x00, 0x00, 0x00, 0xfa, 0xff, 0x45, 0x02, 0x00, 0x11, 0x01, 0xc0, 0xf2, 0x00,
	0x05, 0x01, 0x40, 0x11, 0x00, 0xc0, 0x12, 0x00, 0x38, 0x00, 0xd2, 0x04, 0xff, 0x00, 0xff, 0x00, 0xd2, 0x02, 0x00, 0x00, 0x00, 0x80,
	0x7e, 0x02, 0xc0, 0xc4, 0x87, 0x00, 0x4e, 0x00, 0xc0, 0xc2, 0x15, 0x00, 0x51, 0x2a, 0x01, 0x80, 0x00, 0x00, 0x10, 0x00,
	0x45, 0x01, 0x00, 0x11, 0x00, 0xc0, 0x12, 0x00, 0x05, 0x02, 0x00, 0x11, 0x00, 0xc0, 0xf2, 0x00, 0x38, 0x00,
	0x4a, 0x01, 0xc0, 0xca, 0x24, 0x00, 0x4a, 0x09, 0xc2, 0xcc, 0x24, 0x00, 0x4a, 0x11, 0xc4, 0xce, 0x24, 0x00, 0x4a, 0x19, 0xc6, 0xd0, 0x24, 0x00,
	0x48, 0x04, 0x00, 0x00, 0x09, 0x02, 0x00, 0x00, 0x00, 0x00, 0xf2, 0xff, 0x08, 0x01, 0x00, 0x00 };
/* scene 31: uint u = uint(col.x * 255.0); return float4(float(u & 0xF0u) / 255.0, float((u << 1) & 0xFFu) / 255.0, float(u >> 4) / 15.0, 1); */
static const uint8_t s31[] = {
	0x51, 0x02, 0x41, 0x80, 0x00, 0x00, 0x10, 0x00, 0x92, 0x02, 0xf8, 0x5b, 0x4a, 0x03, 0xc0, 0xc2, 0x04, 0x00, 0xbe, 0x12, 0x08, 0xc0, 0x21, 0x00,
	0x7e, 0x02, 0x84, 0x30, 0x83, 0x30, 0x2e, 0x0a, 0x00, 0x84, 0x05, 0x87, 0x01, 0x00, 0x6e, 0x12, 0x00, 0xc4, 0x05, 0x80, 0x04, 0x00,
	0xbe, 0x02, 0x0a, 0xc0, 0x61, 0x00, 0xbe, 0x0a, 0x0a, 0xc2, 0x61, 0x00, 0xbe, 0x12, 0x0a, 0xc4, 0x61, 0x00,
	0xd2, 0x06, 0x81, 0x80, 0x80, 0x3b, 0x4a, 0x01, 0xc0, 0x86, 0x24, 0x00, 0x4a, 0x09, 0xc2, 0xc6, 0x24, 0x00, 0xd2, 0x06, 0x89, 0x88, 0x88, 0x3d, 0x4a, 0x11, 0xc4, 0xc6, 0x24, 0x00,
	0x52, 0x06, 0x00, 0x00, 0x80, 0x3f, 0x48, 0x00, 0xc2, 0x00, 0x48, 0x0c, 0x00, 0x00, 0x09, 0x02, 0x00, 0x00, 0x00, 0x00, 0xf2, 0xff, 0x08, 0x01, 0x00, 0x00 };
/* scene 35: return float4(float(uint(col.x * 255.0)) / 255.0, 0, 0, 1); */
static const uint8_t s35[] = {
	0x51, 0x02, 0x41, 0x80, 0x00, 0x00, 0x10, 0x00, 0x92, 0x02, 0xf8, 0x5b, 0x4a, 0x03, 0xc0, 0xc2, 0x04, 0x00, 0x52, 0x02, 0x00, 0x00, 0x00, 0x00,
	0xbe, 0x02, 0x08, 0xc0, 0x21, 0x00, 0x52, 0x04, 0x00, 0x00, 0x00, 0x00, 0xbe, 0x02, 0x0a, 0xc0, 0x61, 0x00, 0xd2, 0x06, 0x81, 0x80, 0x80, 0x3b,
	0x4a, 0x01, 0xc0, 0xc6, 0x24, 0x00, 0x52, 0x06, 0x00, 0x00, 0x80, 0x3f, 0x48, 0x00, 0xc2, 0x00, 0x48, 0x0c, 0x00, 0x00, 0x09, 0x02, 0x00, 0x00, 0x00, 0x00, 0xf2, 0xff, 0x08, 0x01, 0x00, 0x00 };

static void runp(struct s8f_ctx * c, const uint8_t * p, unsigned n, const float * col, uint32_t tile)
{
	static f32b vv[4];

	for (int i = 0; i < 4; i++) vv[i] = tob(col[i]);
	memset(c, 0, sizeof(*c));
	c->varying = vv;
	c->tile = tile;
	c->has_tile = 1;
	s8f_run(c, p, n);
}

static void t_f16(void)
{
	/* every half against the exact value computed in double */
	int bad = 0;

	for (uint32_t h = 0; h < 65536; h++) {
		double e = (h >> 10) & 31, m = h & 1023, v;
		f32b f = sf_h2f((uint16_t)h);

		if (e == 31) { if (!isnan(tof(f)) && !isinf(tof(f))) bad++; continue; }
		v = e == 0 ? ldexp(m, -24) : ldexp(1024 + m, (int)e - 25);
		if (h & 0x8000) v = -v;
		if ((double)tof(f) != v) bad++;
		if (sf_f2h(f) != h) bad++;                               /* exact round trip */
	}
	/* round to nearest even: floats between two halves, checked with the exact double distance */
	for (int i = 0; i < 400000; i++) {
		float x = tof((uint32_t)(rnd() << 24) ^ (uint32_t)(rnd() << 8) ^ rnd());
		uint16_t h;
		double best = 1e300;
		uint16_t hb = 0;

		if (isnan(x) || isinf(x) || fabsf(x) > 65000.0f || fabsf(x) < 1e-9f) continue;
		h = sf_f2h(tob(x));
		for (int d = -2; d <= 2; d++) {
			uint32_t hh = (uint32_t)h + d;
			double e, m, v;

			if (hh >= 65536 || ((hh >> 10) & 31) == 31) continue;
			e = (hh >> 10) & 31; m = hh & 1023;
			v = e == 0 ? ldexp(m, -24) : ldexp(1024 + m, (int)e - 25);
			if (hh & 0x8000) v = -v;
			if (fabs(v - (double)x) < best - 1e-300 || (fabs(v - (double)x) == best && !(hh & 1))) { best = fabs(v - (double)x); hb = (uint16_t)hh; }
		}
		if (hb != h) { if (bad < 5) printf("  f2h(%a) = %04x, reference %04x\n", x, h, hb); bad++; }
	}
	CHECK(bad == 0, "f16 conversions: %d mismatches", bad);
	printf("f16 <-> f32: %s\n", bad ? "FAIL" : "all 65536 halves exact, 400000 random floats round to nearest even");
}

static void t_pack(const char * name, const uint8_t * p, unsigned n, uint32_t (*ref)(uint32_t pk, uint32_t tile), int use_tile, int mul_col)
{
	int bad = 0;

	for (int i = 0; i < 2000; i++) {
		float col[4] = { rndf(), rndf(), rndf(), rndf() };
		uint32_t tile = rnd() * 2654435761u;
		struct s8f_ctx c;
		uint32_t pk;
		uint32_t q;
		uint8_t pb[4];

		runp(&c, p, n, col, tile);
		if (use_tile) {
			pb[0] = (tile >> 16) & 255; pb[1] = (tile >> 8) & 255; pb[2] = tile & 255; pb[3] = tile >> 24;     /* ld_tile reads BGRA8 as R G B A */
		} else {
			for (int k = 0; k < 4; k++) pb[k] = q8(col[k]);
		}
		pk = pb[0] | (pb[1] << 8) | (pb[2] << 16) | ((uint32_t)pb[3] << 24);
		q = ref(pk, tile);
		for (int k = 0; k < 4; k++) {
			float want = (float)((q >> (8 * k)) & 255) / 255.0f;

			if (mul_col) want *= col[k];
			if (!c.stored || tof(c.out[k]) != want) { if (bad < 3) printf("  %s col %.3f %.3f %.3f %.3f ch %d: got %.6f want %.6f (unknown 0x%02x)\n", name, col[0], col[1], col[2], col[3], k, tof(c.out[k]), want, c.unknown); bad++; }
		}
	}
	CHECK(bad == 0, "%s", name);
	printf("%s: %s\n", name, bad ? "FAIL" : "2000 random inputs bit-exact against the host reference");
}

static uint32_t ref_g3(uint32_t pk, uint32_t t) { (void)t; return (pk >> 1) & 0x7f7f7f7fu; }
static uint32_t ref_g5(uint32_t pk, uint32_t t) { (void)t; return pk ^ 0xff00ff00u; }
static uint32_t ref_g7(uint32_t pk, uint32_t t) { (void)t; return (pk & 0x00ff00ffu) | 0x80000000u; }

static void t_scene31_35(void)
{
	int bad31 = 0, bad35 = 0;

	for (int i = 0; i < 3000; i++) {
		float col[4] = { (i < 256) ? (i + 0.37f) / 255.0f : rndf(), 0, 0, 1 };
		struct s8f_ctx c;
		uint32_t u = (uint32_t)(col[0] * 255.0f);
		/* Apple's compiler turns `/ 255.0` into a multiply by the float reciprocal (the programs load 0x3b808081 and 0x3d888889 with mov_imm), so the
		 * reference multiplies by (1.0f / 255.0f) and (1.0f / 15.0f) too */
		float r255 = 1.0f / 255.0f, r15 = 1.0f / 15.0f;
		float w31[4] = { (float)(u & 0xF0u) * r255, (float)((u << 1) & 0xFFu) * r255, (float)(u >> 4) * r15, 1.0f };
		float w35[4] = { (float)u * r255, 0, 0, 1 };

		runp(&c, s31, sizeof(s31), col, 0);
		for (int k = 0; k < 4; k++) if (!c.stored || tof(c.out[k]) != w31[k]) { if (bad31 < 3) printf("  s31 col.x %.5f ch %d got %.6f want %.6f unk %02x\n", col[0], k, tof(c.out[k]), w31[k], c.unknown); bad31++; }
		runp(&c, s35, sizeof(s35), col, 0);
		for (int k = 0; k < 4; k++) if (!c.stored || tof(c.out[k]) != w35[k]) { if (bad35 < 3) printf("  s35 col.x %.5f ch %d got %.6f want %.6f unk %02x\n", col[0], k, tof(c.out[k]), w35[k], c.unknown); bad35++; }
	}
	CHECK(bad31 == 0, "scene 31");
	CHECK(bad35 == 0, "scene 35");
	printf("scene 31 (uint bitops: and-imm, bfi, bfeil, convert, f16 imm): %s\n", bad31 ? "FAIL" : "3000 inputs bit-exact");
	printf("scene 35 (uint(x*255)/255: convert f_to_u32 truncation, u32_to_f): %s\n", bad35 ? "FAIL" : "3000 inputs bit-exact");
}

/* the bitop truth tables: and 8, or e, xor 6 were seen in programs; the other nibbles follow the G13 numbering (ASSUMED) */
static void t_bitop(void)
{
	int bad = 0;

	for (int tt = 0; tt < 16; tt++) {
		uint8_t prog[] = { 0x7e, 0x00, 0xc0, 0xc2, (uint8_t)((tt << 4) | 7), 0x00, 0x08, 0x01, 0x00, 0x00 };

		for (int i = 0; i < 200; i++) {
			struct s8f_ctx c;
			uint32_t a = rnd() * 2654435761u, b = rnd() * 40503u + (rnd() << 16), want = 0;

			memset(&c, 0, sizeof(c));
			c.r[0] = a;
			c.r[1] = b;
			s8f_run(&c, prog, sizeof(prog));
			for (int bit = 0; bit < 32; bit++) {
				int x = (a >> bit) & 1, y = (b >> bit) & 1;
				int idx = x * 2 + y;                    /* tt bit idx: 0 = ~a~b, 1 = a~b, 2 = ~ab, 3 = ab */
				int map[4] = { 0, 2, 1, 3 };            /* index (x,y) = (0,0) (0,1) (1,0) (1,1) -> tt bit */

				if ((tt >> map[idx]) & 1) want |= 1u << bit;
			}
			if (c.r[0] != want) bad++;
		}
	}
	CHECK(bad == 0, "bitop");
	printf("bitop (all 16 truth tables, nibble in b4): %s\n", bad ? "FAIL" : "ok");
}

static int walk(const uint8_t * m, unsigned len, unsigned q, unsigned * endp)
{
	unsigned pc = q;

	while (pc + 2 <= len) {
		unsigned n = s8f_len(m + pc, len - pc);

		if (n == 0 || pc + n > len) return 0;
		if (m[pc] == 0x08 && m[pc + 1] == 0x01) { *endp = pc + 4; return 1; }
		pc += n;
	}
	return 0;
}

static void t_resolve(const char * path)
{
	FILE * f = fopen(path, "rb");
	char kind;
	uint32_t entry, len;
	int idx = 0;
	static uint8_t buf[8192];

	if (!f) { printf("corpus not given, MSAA resolve consistency test skipped\n"); return; }
	while (fread(&kind, 1, 1, f) == 1) {
		uint8_t pad[3];

		if (fread(pad, 1, 3, f) != 3 || fread(&entry, 4, 1, f) != 1 || fread(&len, 4, 1, f) != 1 || len > sizeof(buf) || fread(buf, 1, len, f) != len) break;
		if (idx++ == 5 && kind == 'f') {
			unsigned start = entry, end = 0;
			int bad = 0;

			for (unsigned q = 0; q <= entry; q++) if (walk(buf, len, q, &end) && end > entry) { start = q; break; }
			for (int i = 0; i < 500; i++) {
				struct s8f_ctx c;
				uint32_t sm[16];
				double mean[4] = { 0, 0, 0, 0 };
				static const int used[4] = { 0, 1, 4, 5 };   /* sample indexes the four ld_tiles of frag 5 address (b3[3:2] | b5[3:2] << 2) */

				memset(&c, 0, sizeof(c));
				for (int s = 0; s < 16; s++) sm[s] = c.tile_s[s] = rnd() * 2654435761u;
				c.ntile_s = 16;
				s8f_run(&c, buf + start, len - start);
				for (int u = 0; u < 4; u++) {
					mean[0] += ((sm[used[u]] >> 16) & 255) / 255.0 / 4;
					mean[1] += ((sm[used[u]] >> 8) & 255) / 255.0 / 4;
					mean[2] += (sm[used[u]] & 255) / 255.0 / 4;
					mean[3] += (sm[used[u]] >> 24) / 255.0 / 4;
				}
				for (int k = 0; k < 4; k++) if (!c.stored || fabs((double)tof(c.out[k]) - mean[k]) > 2e-3) { if (bad < 3) printf("  resolve ch %d got %.5f want %.5f stored %d unk %02x\n", k, tof(c.out[k]), mean[k], c.stored, c.unknown); bad++; }
			}
			printf("frag 5 (compositor, ASSUMED f16 ffma/ld_tile forms) on 4 synthetic samples: %s\n", bad ? "does NOT equal the 4-sample mean" : "output = mean of the four samples (+-2e-3) for 500 random tiles: dataflow consistent");
			if (bad) fails++;
			break;
		}
	}
	fclose(f);
}

/* scene 19: return float4(1.0/(col.x + 1.0), col.y, col.z, 1) ; scene 21: return float4(min(col.x, 0.5), max(col.y, 0.25), 0, 1) (capture9) */
static const uint8_t s19[] = {
	0x51, 0x02, 0xc1, 0x80, 0x00, 0x00, 0x10, 0x00, 0x8a, 0x03, 0xc0, 0x30, 0x04, 0x00, 0x52, 0x06, 0x00, 0x00, 0x80, 0x3f, 0x0a, 0x01, 0xc0, 0x44,
	0x48, 0x00, 0xc2, 0x00, 0x48, 0x0c, 0x00, 0x00, 0x09, 0x02, 0x00, 0x00, 0x00, 0x00, 0xf2, 0xff, 0x08, 0x01, 0x00, 0x00 };
static const uint8_t s21[] = {
	0x51, 0x02, 0x81, 0x80, 0x00, 0x00, 0x10, 0x00, 0xd2, 0x04, 0x00, 0x00, 0x00, 0x3f, 0x7a, 0x01, 0xc0, 0x20, 0x04, 0xc0, 0xc4, 0x00,
	0xd2, 0x04, 0x00, 0x00, 0x80, 0x3e, 0xfa, 0x09, 0xc2, 0x10, 0x04, 0xc2, 0xc4, 0x00, 0x52, 0x04, 0x00, 0x00, 0x00, 0x00, 0x52, 0x06, 0x00, 0x00, 0x80, 0x3f,
	0x48, 0x00, 0xc2, 0x00, 0x48, 0x0c, 0x00, 0x00, 0x09, 0x02, 0x00, 0x00, 0x00, 0x00, 0xf2, 0xff, 0x08, 0x01, 0x00, 0x00 };

/* the compositor's layer-clear program (fs 0x100619480, capture21): r0 = 0, two compare-selects of h0/h1 into h2/h3, st_tile of h0..h3 -> transparent black whatever the inputs */
static const uint8_t clr[] = {
	0xd2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16, 0x08, 0x40, 0x40, 0x00, 0x40, 0x40, 0x00, 0x16, 0x0c, 0x41, 0x41, 0x00, 0x41, 0x41, 0x00,
	0x48, 0x00, 0xc2, 0x00, 0x48, 0x30, 0x00, 0x00, 0x89, 0x00, 0x80, 0x00, 0x00, 0x00, 0xf0, 0xff, 0x08, 0x01, 0x00, 0x00 };

static void t_clear(void)
{
	int bad = 0;

	for (int i = 0; i < 100; i++) {
		float col[4] = { rndf(), rndf(), rndf(), rndf() };
		struct s8f_ctx c;

		runp(&c, clr, sizeof(clr), col, rnd() * 2654435761u);
		for (int k = 0; k < 4; k++) if (!c.stored || tof(c.out[k]) != 0.0f) bad++;
	}
	CHECK(bad == 0, "layer clear program");
	printf("layer-clear program (16-family compare-select with register operands 0x40/0x41): %s\n", bad ? "FAIL (not transparent black)" : "outputs (0,0,0,0) for any varyings/tile");
}

static void t_scene19_21(void)
{
	t_clear();
	int bad19 = 0, bad21 = 0, badsq = 0;

	for (int i = 0; i < 4000; i++) {
		float col[4] = { rndf() * 2.0f - 0.5f, rndf() * 2.0f - 0.5f, rndf(), 1.0f };
		struct s8f_ctx c;
		float w19[4] = { 1.0f / (col[0] + 1.0f), col[1], col[2], 1.0f };
		float w21[4] = { col[0] < 0.5f ? col[0] : 0.5f, col[1] > 0.25f ? col[1] : 0.25f, 0.0f, 1.0f };

		runp(&c, s19, sizeof(s19), col, 0);
		for (int k = 0; k < 4; k++) if (!c.stored || tof(c.out[k]) != w19[k]) { if (bad19 < 3) printf("  s19 ch %d got %.9g want %.9g\n", k, tof(c.out[k]), w19[k]); bad19++; }
		runp(&c, s21, sizeof(s21), col, 0);
		for (int k = 0; k < 4; k++) if (!c.stored || tof(c.out[k]) != w21[k]) { if (bad21 < 3) printf("  s21 ch %d got %.9g want %.9g\n", k, tof(c.out[k]), w21[k]); bad21++; }
	}
	/* sqrt unit test (the function code of the op is ASSUMED, the arithmetic is checked against the host sqrtf, correctly rounded) */
	for (int i = 0; i < 200000; i++) {
		float x = tof((uint32_t)(rnd() << 24) ^ (uint32_t)(rnd() << 8) ^ rnd()) ;

		if (!(x > 1e-30f && x < 1e30f)) continue;
		if (tof(s8f_sqrt(tob(x))) != sqrtf(x)) badsq++;
	}
	CHECK(bad19 == 0 && bad21 == 0 && badsq == 0, "scene 19/21/sqrt");
	printf("scene 19 (rcp = 0a 01 c0 44, 1/(x+1)): %s\n", bad19 ? "FAIL" : "4000 inputs bit-exact against host division");
	printf("scene 21 (min = 7a, max = fa with the repeated operand pair): %s\n", bad21 ? "FAIL" : "4000 inputs bit-exact against the host");
	printf("s8f_sqrt vs sqrtf (200000 floats): %s\n", badsq ? "FAIL" : "bit-exact");
}

/* ---- texture_sample (0x21) through the ctx texture units: scenes 26 (nearest), 28 (tint), 29 (premultiply) of capture9 on a 4x4 Morton texture ----
 * expected values: independent Morton addressing (y in the low bit) and float math in the test; sampling is the hook of s8gfx_tex.h (nearest only here) */
static uint32_t tex_words[16];
static uint32_t texrd(void * ctx, uint64_t va) { (void)ctx; return tex_words[((va - 0x2000) >> 2) & 15]; }

static unsigned morton4(unsigned x, unsigned y) { return (y & 1) | ((x & 1) << 1) | ((y & 2) << 1) | ((x & 2) << 2); }

static int load_scene(const char * path, uint32_t want, uint8_t * out, uint32_t * n)
{
	FILE * f = fopen(path, "rb");
	uint32_t id, l;

	if (!f) return 0;
	while (fread(&id, 4, 1, f) == 1 && fread(&l, 4, 1, f) == 1) {
		uint8_t tmp[512];

		if (l > sizeof(tmp) || fread(tmp, 1, l, f) != l) break;
		if (id == want) { memcpy(out, tmp, l); *n = l; fclose(f); return 1; }
	}
	fclose(f);
	return 0;
}

static void t_tex(const char * scenes)
{
	int bad[3] = { 0, 0, 0 };
	static const uint32_t sc[3] = { 26, 28, 29 };

	for (int i = 0; i < 16; i++) tex_words[i] = rnd() * 2654435761u;
	for (int s = 0; s < 3; s++) {
		uint8_t prog[512];
		uint32_t n = 0;

		if (!load_scene(scenes, sc[s], prog, &n)) { printf("scene %u not found in %s\n", sc[s], scenes); fails++; continue; }
		for (int i = 0; i < 400; i++) {
			struct s8f_ctx c;
			f32b vv[4];
			float u = (rnd() % 4000) / 4000.0f, v = (rnd() % 4000) / 4000.0f;
			unsigned x = (unsigned)(u * 4.0f), y = (unsigned)(v * 4.0f);
			uint32_t w = tex_words[morton4(x, y)];       /* RGBA8: R in the low byte */
			float t[4] = { (w & 255) / 255.0f, ((w >> 8) & 255) / 255.0f, ((w >> 16) & 255) / 255.0f, (w >> 24) / 255.0f };
			float want[4];

			vv[0] = tob(u); vv[1] = tob(v); vv[2] = tob(0.0f); vv[3] = tob(1.0f);
			memset(&c, 0, sizeof(c));
			c.varying = vv;
			c.ntex = 1;
			s8t_desc_make(&c.texd[0], 0x2000, 4, 4, S8T_FMT_RGBA8, S8T_LAYOUT_MORTON, 0);
			c.trd = texrd;
			s8f_run(&c, prog, n);
			if (s == 0) { for (int k = 0; k < 4; k++) want[k] = t[k]; }
			else if (s == 1) { want[0] = t[0]; want[1] = t[1] * 0.5f; want[2] = t[2] * 0.25f; want[3] = t[3]; }
			else { for (int k = 0; k < 3; k++) want[k] = t[k] * t[3]; want[3] = t[3]; }
			for (int k = 0; k < 4; k++) if (!c.stored || tof(c.out[k]) != want[k]) { if (bad[s] < 3) printf("  scene %u uv %.4f %.4f ch %d got %.6f want %.6f stored %d unk %02x\n", sc[s], u, v, k, tof(c.out[k]), want[k], c.stored, c.unknown); bad[s]++; }
		}
		CHECK(bad[s] == 0, "scene %u texture", sc[s]);
		printf("scene %u (%s, 0x21 + short iter, nearest, 4x4 Morton): %s\n", sc[s], s == 0 ? "tex nearest" : s == 1 ? "tex tint" : "tex premult", bad[s] ? "FAIL" : "400 random uv bit-exact against the host reference (unit mapping ASSUMED)");
	}
}

/* program image start finder on the compositor corpus: block-based records re-embedded behind a 0x40-byte header must come back as block + 0x40 */
static void t_start(const char * path)
{
	FILE * f = fopen(path, "rb");
	char kind;
	uint32_t entry, len;
	int n = 0, ok1 = 0, ok2 = 0, nb = 0, nw = 0;

	if (!f) { printf("corpus_comp.bin not given, start finder test skipped\n"); return; }
	while (fread(&kind, 1, 1, f) == 1) {
		uint8_t pad[3];
		static uint8_t buf[8192], img[8192 + 0x100];
		unsigned st = 0;
		int how;

		if (fread(pad, 1, 3, f) != 3 || fread(&entry, 4, 1, f) != 1 || fread(&len, 4, 1, f) != 1 || len > sizeof(buf) || fread(buf, 1, len, f) != len) break;
		memset(img, 0xee, sizeof(img));
		memcpy(img + 0x80, buf, len);                      /* image: 0x80 junk bytes, 0x40 more would be the header: block = 0x40, code at 0x80 */
		n++;
		/* skip records that do not walk to an end inside the window (cut programs, data records) */
		if (!s8f_walk_lands(buf, len, 0, entry)) continue;
		how = s8f_start_image(img, len + 0x80, 0x40, 0x80 + entry, &st);
		if (pad[0] == 1) { nb++; if (how == 1 && st == 0x80) ok1++; else if (nb < 4) printf("  block record: how %d start %u (want 1 / 128)\n", how, st); }
		else { nw++; how = s8f_start_image(img, len + 0x80, (unsigned)-1, 0x80 + entry, &st); if (how >= 1 && st <= 0x80 + entry && s8f_walk_lands(img, len + 0x80, st, 0x80 + entry)) ok2++; }
	}
	fclose(f);
	CHECK(ok1 == nb && ok2 == nw, "start finder: %d/%d block, %d/%d walk", ok1, nb, ok2, nw);
	printf("s8f_start_image on %d corpus records: %d/%d block images -> block + 0x40, %d/%d headerless images -> a start whose walk lands on the entry: %s\n", n, ok1, nb, ok2, nw, (ok1 == nb && ok2 == nw) ? "ok" : "FAIL");
}

int main(int argc, char ** argv)
{
	t_scene19_21();
	t_tex(argc > 2 ? argv[2] : "../data/scenes.bin");
	t_start(argc > 3 ? argv[3] : "../data/corpus_comp.bin");
	t_f16();
	t_pack("g3 (pack, bfeil shift, and-mask, unpack via device_store/load)", g3, sizeof(g3), ref_g3, 0, 0);
	t_pack("g5 (pack ^ 0xff00ff00)", g5, sizeof(g5), ref_g5, 0, 0);
	t_pack("g7 (ld_tile dst, pack, (p & 0x00ff00ff) + 0x80000000 via bitop+iadd, unpack * col)", g7, sizeof(g7), ref_g7, 1, 1);
	t_scene31_35();
	t_bitop();
	t_resolve(argc > 1 ? argv[1] : "../data/corpus4.bin");
	printf("%s\n", fails ? "isa_test: FAIL" : "isa_test: all passed");
	return fails != 0;
}
