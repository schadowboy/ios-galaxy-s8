/* host test of s8gfx_tex.h: gcc -O1 -I../core tex_test.c -o tex_test -lm ; ./tex_test <capture9 dir> (run via tests/tex_test.sh in WSL)
 * Texture bytes, descriptor and sampler words come from capture9. Expectations are computed independently (texel formula of s8mtl_test.m, host doubles). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "s8gfx_tex.h"

static uint8_t * res1;
static size_t res1len;

static uint32_t
rd(void * ctx, uint64_t va)
{
	uint64_t o = va - 0x100000000ull;
	uint32_t v = 0;

	(void)ctx;
	if (o + 4 <= res1len) {
		memcpy(&v, res1 + o, 4);
	}
	return v;
}

static uint8_t * slurp(const char * dir, const char * name, size_t * len)
{
	char p[1024];
	FILE * f;
	uint8_t * b;

	snprintf(p, sizeof p, "%s/%s", dir, name);
	f = fopen(p, "rb");
	if (!f) {
		perror(p);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc(*len);
	if (fread(b, 1, *len, f) != *len) {
		exit(2);
	}
	fclose(f);
	return b;
}

static float f32(f32b b) { float f; memcpy(&f, &b, 4); return f; }
static f32b bits(float f) { f32b b; memcpy(&b, &f, 4); return b; }

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void texel(int x, int y, int c[4])
{
	c[0] = x * 80 + 15; c[1] = y * 80 + 15; c[2] = 255 - x * 40 - y * 20; c[3] = 255 - (x + y) * 30;
}

int main(int argc, char ** argv)
{
	const char * dir = argc > 1 ? argv[1] : ".";
	size_t l23, l4;
	uint8_t * r23 = slurp(dir, "final_res_23.bin", &l23);
	uint8_t * r4 = slurp(dir, "final_res_4.bin", &l4);
	struct s8t_desc d;
	struct s8t_sampler sn, sl, sr;
	uint32_t tw[6], nw[2] = { 0x000e0000, 0x00c00780 }, lw[2] = { 0x028e0000, 0x00c00780 }, cw[2];
	f32b o[4];
	int x, y, k, i;

	res1 = slurp(dir, "final_res_1.bin", &res1len);
	memcpy(tw, r23, 24);
	memcpy(cw, r4 + 0xa0, 8);
	CHECK(s8t_desc_decode(&d, tw) == 0, "descriptor not recognised");
	CHECK(d.va == 0x100005100ull, "va %llx", (unsigned long long)d.va);
	CHECK(d.width == 4 && d.height == 4, "size %ux%u", d.width, d.height);
	CHECK(d.known, "known flag");
	CHECK(cw[0] == nw[0] && cw[1] == nw[1], "captured nearest sampler words %08x %08x", cw[0], cw[1]);
	s8t_sampler_decode(&sn, nw);
	s8t_sampler_decode(&sl, lw);
	CHECK(!sn.mag_linear && !sn.min_linear && sl.mag_linear && sl.min_linear, "sampler filter bits");
	{
		/* real records of the compositor capture (capture21, session 9): size from the w0/w1 fields, geometry known from the passes / quads / texel memory */
		static const struct { uint32_t w[6]; uint32_t W, H, fmt, lay; uint64_t va; } rec[] = {
			{ { 0xc3055982, 0x00007e04, 0x40298000, 0x80098000, 0x100a69c0, 0x90 }, 153, 64, S8T_FMT_BGRA8, S8T_LAYOUT_LINEAR, 0x100a60000ull },     /* status widget (green rounded rect, 153 px tight rows) */
			{ { 0xfb055982, 0x00009203, 0x40328000, 0x8007c000, 0x100ca940, 0x70 }, 128, 74, S8T_FMT_BGRA8, S8T_LAYOUT_LINEAR, 0x100ca0000ull },    /* target of pass 2 (128x74) */
			{ { 0x1b055982, 0x00009a04, 0x40330000, 0x80080000, 0x100cca10, 0x80 }, 132, 78, S8T_FMT_BGRA8, S8T_LAYOUT_LINEAR, 0x100cc0000ull },    /* target of pass 3 (132x78) */
			{ { 0xbb055982, 0x00036815, 0x40334000, 0x802b4000, 0x100df908, 0x2b0 }, 696, 437, S8T_FMT_BGRA8, S8T_LAYOUT_LINEAR, 0x100cd0000ull },  /* target of pass 5 (696x437) */
			{ { 0xab055982, 0x00036415, 0x402ac000, 0x802b4000, 0x100bd7ac, 0x2b0 }, 694, 435, S8T_FMT_BGRA8, S8T_LAYOUT_LINEAR, 0x100ab0000ull },  /* target of pass 0 (694x435) */
			{ { 0xa8b68002, 0x00009c15, 0x40320010, 0x800ac000, 0, 0xffff0 }, 694, 79, S8T_FMT_A8, S8T_LAYOUT_LINEAR, 0x100c80040ull },         /* date mask: quad 694x79 */
			{ { 0xfb444642, 0x00000007, 0x40324000, 0x801fc000, 0, 0xffff0 }, 256, 1, S8T_FMT_RGBA16F, S8T_LAYOUT_LINEAR, 0x100c90000ull },       /* dither table */
			{ { 0xfcb4000a, 0x00000003, 0x40000840, 0, 0, 0 }, 128, 1, S8T_FMT_R8, S8T_LAYOUT_MORTON, 0x100002100ull },                          /* bell curve table */
			{ { 0xfc0554c2, 0x00171e2c, 0x403b4000, 0xbfffc000, 0, 0xffff0 }, 1440, 2960, S8T_FMT_BGRA8, S8T_LAYOUT_LINEAR, 0x100ed0000ull },    /* display surface */
			{ { 0x03050502, 0x00000000, 0x403ac000, 0x80004000, 0, 0xffff0 }, 1, 1, S8T_FMT_RGBA8, S8T_LAYOUT_LINEAR, 0x100eb0000ull },         /* 1x1 white */
		};
		struct s8t_desc t;
		unsigned ri;

		for (ri = 0; ri < sizeof(rec) / sizeof(rec[0]); ri++) {
			CHECK(s8t_desc_decode(&t, rec[ri].w) == 0, "record %u not recognised", ri);
			CHECK(t.width == rec[ri].W && t.height == rec[ri].H, "record %u size %ux%u vs %ux%u", ri, t.width, t.height, rec[ri].W, rec[ri].H);
			CHECK(t.fmt == rec[ri].fmt && t.layout == rec[ri].lay, "record %u fmt %u layout %u", ri, t.fmt, t.layout);
			CHECK(t.va == rec[ri].va || ri == 5, "record %u va %llx vs %llx", ri, (unsigned long long)t.va, (unsigned long long)rec[ri].va);
		}
	}
	{
		/* half -> float and the RGBA16F fetch: 1.0, 0.5, -2.0, 0.999 (0x3bfc) */
		static uint32_t ftab[2] = { 0x38003c00u, 0x3bfcc000u };
		struct s8t_desc fd;

		CHECK(s8t_half(0x3c00) == 0x3f800000u && s8t_half(0x3800) == 0x3f000000u && s8t_half(0xc000) == 0xc0000000u, "half conversion");
		(void)ftab; (void)fd;
	}

	/* Morton addressing against the captured bytes: texel (x,y) at the index of the interleave, pixel formula of the test app */
	for (y = 0; y < 4; y++) {
		for (x = 0; x < 4; x++) {
			int c[4];
			uint32_t g[4];

			texel(x, y, c);
			s8t_fetch(&d, rd, 0, x, y, g);
			for (k = 0; k < 4; k++) {
				CHECK((int)g[k] == c[k], "fetch (%d,%d) ch %d: %u vs %d", x, y, k, g[k], c[k]);
			}
		}
	}

	/* nearest at texel centres = the texel, for both filters; bilinear at the centres has zero weights on the neighbours */
	for (y = 0; y < 4; y++) {
		for (x = 0; x < 4; x++) {
			int c[4];

			texel(x, y, c);
			for (i = 0; i < 2; i++) {
				s8t_sample(&d, i ? &sl : &sn, bits((x + 0.5f) / 4), bits((y + 0.5f) / 4), rd, 0, o);
				for (k = 0; k < 4; k++) {
					CHECK(o[k] == bits(c[k] / 255.0f), "centre (%d,%d) %s ch %d: %08x vs %08x", x, y, i ? "linear" : "nearest", k, o[k], bits(c[k] / 255.0f));
				}
			}
		}
	}

	/* bilinear at the corner midpoints u = i/4, v = j/4 (i, j = 1..3) = the average of the 4 texels around */
	for (y = 1; y < 4; y++) {
		for (x = 1; x < 4; x++) {
			int a[4], b[4], c[4], e[4];

			texel(x - 1, y - 1, a); texel(x, y - 1, b); texel(x - 1, y, c); texel(x, y, e);
			s8t_sample(&d, &sl, bits(x / 4.0f), bits(y / 4.0f), rd, 0, o);
			for (k = 0; k < 4; k++) {
				int sum = a[k] + b[k] + c[k] + e[k];
				f32b want = sf_div(sf_from_i64(sum), sf_from_i64(1020));

				CHECK(o[k] == want, "corner midpoint (%d,%d) ch %d: %08x vs %08x", x, y, k, o[k], want);
			}
		}
	}

	/* clamp at the borders: u,v outside [0,1] return the edge texel (both filters) */
	for (i = 0; i < 2; i++) {
		int c[4];

		texel(3, 0, c);
		s8t_sample(&d, i ? &sl : &sn, bits(1.7f), bits(-0.3f), rd, 0, o);
		for (k = 0; k < 4; k++) {
			CHECK(o[k] == bits(c[k] / 255.0f), "clamp %d ch %d", i, k);
		}
	}

	/* repeat: u + 1 gives the same result as u */
	sr = sl;
	sr.wrap_s = sr.wrap_t = S8T_WRAP_REPEAT;
	for (i = 0; i < 20; i++) {
		float uu = 0.0371f + 0.049f * i, vv = 0.91f - 0.043f * i;
		f32b a[4], b[4];

		s8t_sample(&d, &sr, bits(uu), bits(vv), rd, 0, a);
		s8t_sample(&d, &sr, bits(uu + 1.0f), bits(vv - 2.0f), rd, 0, b);
		for (k = 0; k < 4; k++) {
			/* uu + 1 loses low mantissa bits in single precision: compare with a tolerance of 1/200 */
			CHECK(fabsf(f32(a[k]) - f32(b[k])) < 0.006f, "repeat i=%d ch %d: %f vs %f", i, k, f32(a[k]), f32(b[k]));
		}
	}

	/* random coordinates against a double precision reference (nearest exact, bilinear within the 8-bit weight quantisation) */
	srand(1);
	for (i = 0; i < 2000; i++) {
		double u = (rand() % 6000 - 1000) / 4000.0, v = (rand() % 6000 - 1000) / 4000.0;
		int lin = i & 1;
		double ref[4];

		s8t_sample(&d, lin ? &sl : &sn, bits((float)u), bits((float)v), rd, 0, o);
		if (!lin) {
			int xi = (int)floor(u * 4), yi = (int)floor(v * 4), c[4];

			xi = xi < 0 ? 0 : xi > 3 ? 3 : xi; yi = yi < 0 ? 0 : yi > 3 ? 3 : yi;
			texel(xi, yi, c);
			for (k = 0; k < 4; k++) {
				float fu = (float)u * 4;
				/* skip values within 1e-4 of a texel boundary (16.16 rounding differs from the double floor) */
				if (fabs(fu - round(fu)) < 1e-4 || fabs((float)v * 4 - round((float)v * 4)) < 1e-4) {
					break;
				}
				CHECK(o[k] == bits(c[k] / 255.0f), "random nearest u=%f v=%f ch %d", u, v, k);
			}
		} else {
			double px = u * 4 - 0.5, py = v * 4 - 0.5;
			int x0 = (int)floor(px), y0 = (int)floor(py);
			double fx = px - x0, fy = py - y0;

			for (k = 0; k < 4; k++) {
				double s = 0;
				int t, c[4];

				for (t = 0; t < 4; t++) {
					int tx = x0 + (t & 1), ty = y0 + (t >> 1);
					double wgt = ((t & 1) ? fx : 1 - fx) * ((t >> 1) ? fy : 1 - fy);

					tx = tx < 0 ? 0 : tx > 3 ? 3 : tx; ty = ty < 0 ? 0 : ty > 3 ? 3 : ty;
					texel(tx, ty, c);
					s += wgt * c[k];
				}
				ref[k] = s / 255.0;
				CHECK(fabs(f32(o[k]) - ref[k]) < 2.0 / 255.0, "random linear u=%f v=%f ch %d: %f vs %f", u, v, k, f32(o[k]), ref[k]);
			}
		}
	}

	/* generic layouts: 8x4 Morton (y low bit, x continues linearly above the common 2 bits) and linear */
	{
		struct s8t_desc w;

		s8t_desc_make(&w, 0x100000000ull, 8, 4, S8T_FMT_RGBA8, S8T_LAYOUT_MORTON, 0);
		CHECK(s8t_texel_offset(&w, 1, 0) == 8, "8x4 offset (1,0)");
		CHECK(s8t_texel_offset(&w, 4, 0) == 64, "8x4 offset (4,0) = %llu", (unsigned long long)s8t_texel_offset(&w, 4, 0));
		s8t_desc_make(&w, 0x100000000ull, 8, 4, S8T_FMT_RGBA8, S8T_LAYOUT_LINEAR, 64);
		CHECK(s8t_texel_offset(&w, 1, 2) == 132, "linear offset");
	}

	printf("tex_test: %d checks, %d failures\n", checks, fails);
	return fails != 0;
}
