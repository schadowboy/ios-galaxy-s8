/*
 * nfp_e2e.c: end to end, the real shader interpreter (s8gfx_frag.h + s8gfx_tex.h) and the rasteriser (s8gfx_rast.h) run twice with the
 * sf_* names redirected by S8_NATIVE_FP=1: s8nfp_enable(0) = integer soft float, s8nfp_enable(1) inside s8_fp_begin/end = native FP.
 * Every program of the corpora (records: kind, pad[3], entry, len, code) is run on several synthetic contexts; the whole s8f_ctx must be
 * byte identical. Build + run: nfp_e2e.sh
 */
#define S8_NFP_USER 1
#define S8_NATIVE_FP 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s8gfx_sf.h"
#include "s8gfx_nfp.h"
#include "s8gfx_rast.h"
#include "s8gfx_frag.h"

static uint64_t rs = 0x1234567887654321ULL;
static uint64_t rnd64(void) { rs ^= rs >> 12; rs ^= rs << 25; rs ^= rs >> 27; return rs * 0x2545F4914F6CDD1DULL; }
static uint32_t rnd(void) { return (uint32_t)(rnd64() >> 32); }

static uint32_t texrd(void * ctx, uint64_t va) { (void)ctx; return (uint32_t)(va * 2654435761u) ^ 0x80402010u; }
static uint32_t vrd(void * ctx, uint64_t va) { (void)ctx; return (uint32_t)(va * 40503u) ^ 0x3f000000u; }

/* a float in a range that exercises normal paths: mostly [-2, 2], sometimes tiny / huge / special */
static f32b rf(void)
{
	switch (rnd() % 12) {
	case 0: return rnd();                                                                  /* anything, NaN/denormal included */
	case 1: return (rnd() & 0x80000000u) | ((rnd() % 40 + 1) << 23) | (rnd() & 0x7fffff);  /* tiny */
	case 2: return (rnd() & 0x80000000u) | ((rnd() % 40 + 214) << 23) | (rnd() & 0x7fffff); /* huge */
	case 3: return 0x3f800000u;
	case 4: return 0;
	default: return (rnd() & 0x80000000u) | ((120 + rnd() % 9) << 23) | (rnd() & 0x7fffff);
	}
}

struct hctx { uint32_t * h; };
static void hemit(void * ctx, int px, int py, const f32b * v)
{
	uint32_t * hh = ((struct hctx *)ctx)->h;
	int k;

	*hh = (*hh ^ (uint32_t)px) * 16777619u;
	*hh = (*hh ^ (uint32_t)py) * 16777619u;
	for (k = 0; k < 4; k++) {
		*hh = (*hh ^ v[k]) * 16777619u;
		*hh = (*hh ^ sf_to_unorm8(v[k])) * 16777619u;
	}
}

static void setup(struct s8f_ctx * c, f32b * vv, int variant)
{
	int i;

	memset(c, 0, sizeof(*c));
	for (i = 0; i < 8; i++) vv[i] = variant == 0 ? (0x3e99999au + i * 0x100000u) : rf();
	c->varying = vv;
	c->tile = rnd();
	c->has_tile = 1;
	for (i = 0; i < S8F_NUNI; i++) c->uni[i] = variant < 2 ? (i & 1 ? 0x3c003c00u : 0x3f000000u) : (rnd() & 1 ? rf() : (rnd() & 0x7bff7bffu));
	c->sr[48] = rnd() & 31;
	c->sr[49] = rnd() & 31;
	c->ntex = 1;
	s8t_desc_make(&c->texd[0], 0x1000, 16, 16, S8T_FMT_RGBA8, S8T_LAYOUT_MORTON, 0);
	c->texs[0].mag_linear = c->texs[0].min_linear = variant & 1;
	c->trd = texrd;
	c->rd = vrd;
	c->fetch = vv;
	c->nfetch = 4;
	c->vid = rnd() & 255;
}

int main(int argc, char ** argv)
{
	long runs = 0, diff = 0, nanonly = 0, progs = 0, stored = 0;
	int a;

	for (a = 1; a < argc; a++) {
		FILE * f = fopen(argv[a], "rb");
		char kind;
		uint32_t entry, len;
		static uint8_t buf[8192];
		long fprog = 0, fdiff = 0;

		if (!f) { printf("cannot open %s\n", argv[a]); continue; }
		while (fread(&kind, 1, 1, f) == 1) {
			uint8_t pad[3];

			if (fread(pad, 1, 3, f) != 3 || fread(&entry, 4, 1, f) != 1 || fread(&len, 4, 1, f) != 1 || len > sizeof(buf) || fread(buf, 1, len, f) != len) break;
			fprog++;
			for (int v = 0; v < 6; v++) {
				static struct s8f_ctx c1, c2;
				f32b vv[8];
				struct s8_fpsave fs;
				uint64_t seed = rs;

				setup(&c1, vv, v);
				memcpy(&c2, &c1, sizeof(c1));
				c2.varying = c1.varying;
				s8nfp_enable(0);
				s8f_run(&c1, buf, len);
				s8nfp_enable(1);
				s8_fp_begin(&fs);
				s8f_run(&c2, buf, len);
				s8_fp_end(&fs);
				s8nfp_enable(0);
				(void)seed;
				runs++;
				stored += c1.stored;
				if (memcmp(&c1, &c2, sizeof(c1))) {
					int nan_only = 1, k;

					/* tolerate only NaN payload/sign differences in the register files */
					for (k = 0; k < S8F_REGS; k++) if (c1.r[k] != c2.r[k] && !(sf_isnan(c1.r[k]) && sf_isnan(c2.r[k]))) nan_only = 0;
					for (k = 0; k < S8F_REGS * 2; k++) if (c1.h[k] != c2.h[k] && !(sf_isnan(c1.h[k]) && sf_isnan(c2.h[k]))) nan_only = 0;
					for (k = 0; k < 4; k++) if (c1.out[k] != c2.out[k] && !(sf_isnan(c1.out[k]) && sf_isnan(c2.out[k]))) nan_only = 0;
					for (k = 0; k < 16; k++) if (c1.exp[k] != c2.exp[k] && !(sf_isnan(c1.exp[k]) && sf_isnan(c2.exp[k]))) nan_only = 0;
					if (c1.stored != c2.stored || c1.pc_end != c2.pc_end || c1.unknown != c2.unknown || memcmp(c1.scratch, c2.scratch, sizeof(c1.scratch))) nan_only = 0;
					if (nan_only) {
						nanonly++;
					} else {
						diff++;
						fdiff++;
						if (diff <= 8) {
							printf("  %s prog %ld variant %d: DIFF stored %d/%d pc_end %u/%u out %08x %08x %08x %08x | %08x %08x %08x %08x\n", argv[a], fprog - 1, v, c1.stored, c2.stored, c1.pc_end, c2.pc_end,
							    c1.out[0], c1.out[1], c1.out[2], c1.out[3], c2.out[0], c2.out[1], c2.out[2], c2.out[3]);
						}
					}
				}
			}
		}
		fclose(f);
		printf("%s: %ld programs, %ld differing runs\n", argv[a], fprog, fdiff);
		progs += fprog;
	}
	/* rasteriser: one triangle with varyings, hash of every emitted pixel, both modes */
	{
		struct s8r_vtx va, vb, vc;
		static uint32_t hs[2];

		for (int m = 0; m < 2; m++) {
			struct s8_fpsave fs;
			uint32_t h = 2166136261u;
			struct hctx hc = { &h };

			memset(&va, 0, sizeof(va)); memset(&vb, 0, sizeof(vb)); memset(&vc, 0, sizeof(vc));
			s8nfp_enable(m);
			s8_fp_begin(&fs);
			va.x = sf_from_i64(3); va.y = sf_from_i64(2); va.iw = sf_div(SF_ONE, sf_from_i64(2));
			vb.x = sf_from_i64(250); vb.y = sf_from_i64(40); vb.iw = sf_div(SF_ONE, sf_from_i64(3));
			vc.x = sf_from_i64(90); vc.y = sf_from_i64(300); vc.iw = sf_div(SF_ONE, sf_from_i64(5));
			for (int k = 0; k < 4; k++) { va.v[k] = sf_mul(sf_from_i64(k + 1), va.iw); vb.v[k] = sf_mul(sf_from_i64(7 - k), vb.iw); vc.v[k] = sf_from_i64(k * k); }
			s8r_tri(&va, &vb, &vc, 4, 0, 0, 320, 320, hemit, &hc);
			s8_fp_end(&fs);
			hs[m] = h;
		}
		s8nfp_enable(0);
		printf("rasteriser triangle hash soft %08x native %08x %s\n", hs[0], hs[1], hs[0] == hs[1] ? "(identical)" : "DIFFERENT");
		if (hs[0] != hs[1]) diff++;
	}
	printf("e2e: %ld programs, %ld runs (%ld stored a pixel), %ld differing runs, %ld differing only in NaN payloads\n", progs, runs, stored, diff, nanonly);
	return diff != 0;
}
