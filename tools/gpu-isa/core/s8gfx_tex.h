/*
 * Texture sampler for the A11 (G10) AGX stand-in: integer-only (soft float of s8gfx_sf.h), no libc, builds into the kernel and the host test
 * (tools/gpu-isa/tests/tex_test.c). Analysis and evidence levels: tools/gpu-isa/analysis/texture-state.md.
 *
 *   s8t_desc_decode()    6-word texture state record (as pushed by the 0x17 push record) -> struct s8t_desc
 *   s8t_sampler_decode() 2-word sampler state (as pushed by the 0x13 push record)        -> struct s8t_sampler
 *   s8t_sample()         u, v (normalised, float bits) -> RGBA float bits, nearest or bilinear, level 0 only
 *
 * Verified on captured data: the texel address (word2 << 2 | bit 30 = VA bit 32), width / height fields of word0/word1 (capture9 4x4 and capture21: 153x64, 128x74,
 * 696x437, 694x79, 1440x2960, 256x1, 128x1), Morton (y in the LOW bit) for the 4x4 test texture and linear tight packing for the BGRA8 widget, the format codes
 * listed at s8t_desc_decode (RGBA8, BGRA8, R8 table, A8 masks, RGBA16F table), and the nearest / linear bit of the sampler word. Wrap modes, min/mag split, tiling
 * of large Morton textures, mips and the stride word are NOT decoded; the struct fields can be filled in by hand (s8t_desc_make).
 * Filtering model (assumption, no hardware oracle exists yet): coordinates and texel positions in 16.16 fixed point, bilinear weights truncated to
 * 8 bits, the result sum(w * texel8) / (65536 * 255) rounded to nearest by the soft float divide.
 */
#ifndef S8GFX_TEX_H
#define S8GFX_TEX_H

#include "s8gfx_sf.h"

enum { S8T_FMT_RGBA8 = 0, S8T_FMT_BGRA8 = 1, S8T_FMT_R8 = 2, S8T_FMT_A8 = 3, S8T_FMT_RGBA16F = 4, S8T_FMT_LA8 = 5 };
/* A8: one byte per texel returned in the ALPHA channel (0,0,0,v): the compositor's 0x8002 glyph / mask textures. RGBA16F: 8 bytes per texel (the dither table, code 0x4642).
 * LA8: 2 bytes per texel, returned as (L, L, L, A) (IOSurface LA08 icons, code 0x0142; byte order L then A is an assumption). */
enum { S8T_LAYOUT_MORTON = 0, S8T_LAYOUT_LINEAR = 1 };
enum { S8T_WRAP_CLAMP_EDGE = 0, S8T_WRAP_REPEAT = 1, S8T_WRAP_MIRROR = 2, S8T_WRAP_ZERO = 3 };

struct s8t_desc {
	uint64_t va;            /* address of level 0 */
	uint32_t width, height; /* texels */
	uint32_t levels;        /* mip levels (only level 0 is sampled) */
	uint32_t fmt;           /* S8T_FMT_* */
	uint32_t layout;        /* S8T_LAYOUT_* */
	uint32_t pitch;         /* bytes per row, linear layout only */
	int known;              /* 1 when the size/format came from a record the decoder recognises */
};

struct s8t_sampler {
	int mag_linear;
	int min_linear;
	uint32_t wrap_s, wrap_t;        /* S8T_WRAP_* */
};

static inline uint32_t
s8t_bpp(uint32_t fmt)
{
	return (fmt == S8T_FMT_R8 || fmt == S8T_FMT_A8) ? 1 : fmt == S8T_FMT_LA8 ? 2 : fmt == S8T_FMT_RGBA16F ? 8 : 4;
}

static inline void
s8t_desc_make(struct s8t_desc * d, uint64_t va, uint32_t w, uint32_t h, uint32_t fmt, uint32_t layout, uint32_t pitch)
{
	d->va = va;
	d->width = w;
	d->height = h;
	d->levels = 1;
	d->fmt = fmt;
	d->layout = layout;
	d->pitch = pitch ? pitch : w * s8t_bpp(fmt);
	d->known = 0;
}

/*
 * Decode a texture state record (6 words; capture9 and the real compositor capture21 fix this layout, see analysis/texture-state.md sec. 9):
 *   word0 bits 0-15  = type code: bit 3 = layout (1 Morton, 0 linear), (code & ~8) selects the format: 0x0502 RGBA8, 0x5982 BGRA8, 0x0002 R8, 0x8002 A8,
 *                      0x4642 RGBA16F, 0x0142 LA8, 0x54c2 BGRA8 (the display surface); unlisted codes are taken as BGRA8 and known = 0
 *   (word0 >> 27) | (word1 & 0x1ff) << 5   = width - 1   (14 bits)
 *   (word1 >> 9) & 0x3fff                  = height - 1  (14 bits)
 *   word2 = VA >> 2, bit 30 = VA bit 32;  word3 (stride-like, undecoded), word4/5 = end-of-pixels pointer (VA >> 4) and size of the lossless-compression metadata, ignored
 * Linear textures are tightly packed here (pitch = width * bpp, verified on the 153x64 BGRA8 widget); the caller overrides pitch / size (s8t_desc_make) for passes' targets.
 * Returns 0 when the record is recognised (word2 has VA bit 32 and (word0 & 7) == 2), -1 otherwise.
 */
static inline int
s8t_desc_decode(struct s8t_desc * d, const uint32_t w[6])
{
	uint32_t code = w[0] & 0xffff;

	d->va = ((uint64_t)(w[2] & 0x3fffffffu) << 2) | ((w[2] & 0x40000000u) ? 0x100000000ull : 0);
	d->fmt = S8T_FMT_BGRA8;
	d->layout = (code & 8) ? S8T_LAYOUT_MORTON : S8T_LAYOUT_LINEAR;
	d->levels = 1;
	d->known = 1;
	switch (code & ~8u) {
	case 0x0502: d->fmt = S8T_FMT_RGBA8; break;
	case 0x5982: case 0x54c2: break;
	case 0x0002: d->fmt = S8T_FMT_R8; break;
	case 0x8002: d->fmt = S8T_FMT_A8; break;
	case 0x4642: d->fmt = S8T_FMT_RGBA16F; break;
	case 0x0142: d->fmt = S8T_FMT_LA8; break;
	default: d->known = 0; break;
	}
	d->width = ((w[0] >> 27) | ((w[1] & 0x1ffu) << 5)) + 1;
	d->height = ((w[1] >> 9) & 0x3fffu) + 1;
	d->pitch = d->width * s8t_bpp(d->fmt);
	return ((w[0] & 7) == 2 && (w[2] & 0x40000000u)) ? 0 : -1;
}

/* sampler state, two words. word0 bit 23 = linear (magnify), bit 25 = linear (minify): both set by filter::linear, both clear for nearest [verified as a pair]. Wrap modes: not decoded (clamp_to_edge). */
static inline void
s8t_sampler_decode(struct s8t_sampler * s, const uint32_t w[2])
{
	s->mag_linear = (w[0] >> 23) & 1;
	s->min_linear = (w[0] >> 25) & 1;
	s->wrap_s = s->wrap_t = S8T_WRAP_CLAMP_EDGE;
}

static inline uint32_t
s8t_log2(uint32_t v)
{
	uint32_t k = 0;

	while ((1u << k) < v && k < 31) {
		k++;
	}
	return k;
}

/* byte offset of texel (x, y) from the level base */
static inline uint64_t
s8t_texel_offset(const struct s8t_desc * d, uint32_t x, uint32_t y)
{
	uint64_t idx;

	if (d->layout == S8T_LAYOUT_LINEAR) {
		return (uint64_t)y * d->pitch + (uint64_t)x * s8t_bpp(d->fmt);
	}
	/* Morton / twiddle, y in the LOW bit: interleave the low min(lw, lh) bits of y and x, the remaining bits of the longer side follow linearly */
	{
		uint32_t lw = s8t_log2(d->width), lh = s8t_log2(d->height);
		uint32_t n = lw < lh ? lw : lh;
		uint32_t b;

		idx = 0;
		for (b = 0; b < n; b++) {
			idx |= (uint64_t)((y >> b) & 1) << (2 * b);
			idx |= (uint64_t)((x >> b) & 1) << (2 * b + 1);
		}
		if (lw > lh) {
			idx |= (uint64_t)(x >> n) << (2 * n);
		} else if (lh > lw) {
			idx |= (uint64_t)(y >> n) << (2 * n);
		}
	}
	return idx * s8t_bpp(d->fmt);
}

/* apply a wrap mode to texel coordinate c in a dimension of n texels; returns -1 for "outside, transparent black" */
static inline int32_t
s8t_wrap(int32_t c, int32_t n, uint32_t mode)
{
	switch (mode) {
	case S8T_WRAP_REPEAT:
		c %= n;
		return c < 0 ? c + n : c;
	case S8T_WRAP_MIRROR: {
		int32_t p = 2 * n;

		c %= p;
		if (c < 0) {
			c += p;
		}
		return c < n ? c : p - 1 - c;
	}
	case S8T_WRAP_ZERO:
		return (c < 0 || c >= n) ? -1 : c;
	default:
		return c < 0 ? 0 : (c >= n ? n - 1 : c);
	}
}

/* the four channels of texel (x, y) as 8-bit values in RGBA order; outside (x or y < 0) = 0 */
static inline void
s8t_fetch(const struct s8t_desc * d, uint32_t (*rd)(void *, uint64_t va), void * ctx, int32_t x, int32_t y, uint32_t c[4])
{
	uint64_t a;
	uint32_t v;

	if (x < 0 || y < 0) {
		c[0] = c[1] = c[2] = c[3] = 0;
		return;
	}
	a = d->va + s8t_texel_offset(d, (uint32_t)x, (uint32_t)y);
	v = rd(ctx, a & ~3ull);
	if (d->fmt == S8T_FMT_A8) {
		c[0] = c[1] = c[2] = 0;
		c[3] = (v >> (8 * (a & 3))) & 0xff;
	} else if (d->fmt == S8T_FMT_LA8) {
		uint32_t h = (v >> (8 * (a & 3))) & 0xffff;

		c[0] = c[1] = c[2] = h & 0xff;
		c[3] = h >> 8;
	} else if (d->fmt == S8T_FMT_R8) {
		c[0] = (v >> (8 * (a & 3))) & 0xff;
		c[1] = c[2] = 0;
		c[3] = 255;
	} else if (d->fmt == S8T_FMT_BGRA8) {
		c[0] = (v >> 16) & 0xff;
		c[1] = (v >> 8) & 0xff;
		c[2] = v & 0xff;
		c[3] = v >> 24;
	} else {
		c[0] = v & 0xff;
		c[1] = (v >> 8) & 0xff;
		c[2] = (v >> 16) & 0xff;
		c[3] = v >> 24;
	}
}

/* IEEE half -> float bits (denormal halves flush to zero like the soft float does) */
static inline f32b
s8t_half(uint32_t h)
{
	uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 0x3ff;

	if (e == 0) {
		return s << 31;
	}
	if (e == 31) {
		return (s << 31) | 0x7f800000u | (m << 13);
	}
	return (s << 31) | ((e + 112) << 23) | (m << 13);
}

/* RGBA16F texel (x, y) as float bits; outside (x or y < 0) = 0 */
static inline void
s8t_fetch_f(const struct s8t_desc * d, uint32_t (*rd)(void *, uint64_t va), void * ctx, int32_t x, int32_t y, f32b c[4])
{
	uint64_t a;
	uint32_t lo, hi;

	if (x < 0 || y < 0) {
		c[0] = c[1] = c[2] = c[3] = 0;
		return;
	}
	a = d->va + s8t_texel_offset(d, (uint32_t)x, (uint32_t)y);
	lo = rd(ctx, a);
	hi = rd(ctx, a + 4);
	c[0] = s8t_half(lo & 0xffff);
	c[1] = s8t_half(lo >> 16);
	c[2] = s8t_half(hi & 0xffff);
	c[3] = s8t_half(hi >> 16);
}

/* a * 2^16 as int32 (rounded to nearest at 2^-16, saturating); >> 16 of the result is the floor */
static inline int32_t
s8t_fix16_floor(f32b a)
{
	uint32_t e = (a >> 23) & 0xff;

	if (e > 127 + 14) {                     /* |a| >= 2^15: far outside the texture, saturate */
		return (a >> 31) ? INT32_MIN : INT32_MAX;
	}
	return sf_to_fixed(a, 16);
}

/*
 * Sample level 0. u, v are normalised coordinates (float bits). out = R, G, B, A as float bits in [0, 1].
 * Nearest: texel floor(u * w). Bilinear: position u * w - 0.5, 2x2 taps with 8-bit weights.
 */
static inline void
s8t_sample(const struct s8t_desc * d, const struct s8t_sampler * s, f32b u, f32b v, uint32_t (*rd)(void *, uint64_t va), void * ctx, f32b out[4])
{
	int32_t W = (int32_t)d->width, H = (int32_t)d->height;
	f32b fw = sf_from_i64(W), fh = sf_from_i64(H);
	f32b half = 0x3f000000u;
	f32b su = sf_mul(u, fw), sv = sf_mul(v, fh);        /* texel space */
	int linear = s->mag_linear || s->min_linear;        /* level 0 only: magnification and minification are told apart by the LOD, which this stand-in does not compute */
	uint32_t c[4];
	int k;

	if (W <= 0 || H <= 0) {
		for (k = 0; k < 4; k++) {
			out[k] = SF_ZERO;
		}
		return;
	}
	if (!linear) {
		int32_t fx = s8t_fix16_floor(su), fy = s8t_fix16_floor(sv);
		int32_t x = fx >> 16, y = fy >> 16;     /* arithmetic shift = floor */

		if (d->fmt == S8T_FMT_RGBA16F) {
			s8t_fetch_f(d, rd, ctx, s8t_wrap(x, W, s->wrap_s), s8t_wrap(y, H, s->wrap_t), out);
			return;
		}
		s8t_fetch(d, rd, ctx, s8t_wrap(x, W, s->wrap_s), s8t_wrap(y, H, s->wrap_t), c);
		for (k = 0; k < 4; k++) {
			out[k] = sf_div(sf_from_i64(c[k]), 0x437f0000u);
		}
		return;
	}
	{
		int32_t fx = s8t_fix16_floor(sf_sub(su, half)), fy = s8t_fix16_floor(sf_sub(sv, half));
		int32_t x0 = fx >> 16, y0 = fy >> 16;
		uint32_t wx = ((uint32_t)fx >> 8) & 0xff, wy = ((uint32_t)fy >> 8) & 0xff;   /* 8-bit weights, truncated */
		uint32_t w00 = (256 - wx) * (256 - wy), w10 = wx * (256 - wy), w01 = (256 - wx) * wy, w11 = wx * wy;
		uint32_t acc[4] = { 0, 0, 0, 0 };
		uint32_t t[4];
		int32_t xs[2], ys[2];
		uint32_t wt[4];
		int i;

		xs[0] = s8t_wrap(x0, W, s->wrap_s);
		xs[1] = s8t_wrap(x0 + 1, W, s->wrap_s);
		ys[0] = s8t_wrap(y0, H, s->wrap_t);
		ys[1] = s8t_wrap(y0 + 1, H, s->wrap_t);
		wt[0] = w00; wt[1] = w10; wt[2] = w01; wt[3] = w11;
		if (d->fmt == S8T_FMT_RGBA16F) {
			f32b facc[4] = { 0, 0, 0, 0 }, tf[4];

			for (i = 0; i < 4; i++) {
				f32b wf;

				if (wt[i] == 0) {
					continue;
				}
				wf = sf_div(sf_from_i64(wt[i]), sf_from_i64(65536));
				s8t_fetch_f(d, rd, ctx, xs[i & 1], ys[i >> 1], tf);
				for (k = 0; k < 4; k++) {
					facc[k] = sf_fma(tf[k], wf, facc[k]);
				}
			}
			for (k = 0; k < 4; k++) {
				out[k] = facc[k];
			}
			return;
		}
		for (i = 0; i < 4; i++) {
			if (wt[i] == 0) {
				continue;
			}
			s8t_fetch(d, rd, ctx, xs[i & 1], ys[i >> 1], t);
			for (k = 0; k < 4; k++) {
				acc[k] += t[k] * wt[i];
			}
		}
		for (k = 0; k < 4; k++) {
			out[k] = sf_div(sf_from_i64(acc[k]), sf_from_i64(65536 * 255ll));
		}
	}
}

#endif /* S8GFX_TEX_H */
