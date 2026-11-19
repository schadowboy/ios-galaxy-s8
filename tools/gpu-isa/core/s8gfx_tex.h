/*
 * Texture sampler for the A11 (G10) AGX stand-in: integer-only (soft float of s8gfx_sf.h), no libc, builds into the kernel and the host test
 * (tools/gpu-isa/tests/tex_test.c). Analysis and evidence levels: tools/gpu-isa/analysis/texture-state.md.
 *
 *   s8t_desc_decode()    6-word texture state record (as pushed by the 0x17 push record) -> struct s8t_desc
 *   s8t_sampler_decode() 2-word sampler state (as pushed by the 0x13 push record)        -> struct s8t_sampler
 *   s8t_sample()         u, v (normalised, float bits) -> RGBA float bits, nearest or bilinear, level 0 only
 *
 * What is verified on captured data (capture9, 4x4 RGBA8): the texel address (word2 << 2), Morton addressing with y in the LOW bit, RGBA8 byte order,
 * and the nearest / linear bit of the sampler word. Everything else (size field, format field, wrap modes, min/mag split, tiling of large textures)
 * is a HYPOTHESIS; the struct fields can be filled in by hand (s8t_desc_make) when the caller knows better (e.g. from the IOGPU resource size).
 * Filtering model (assumption, no hardware oracle exists yet): coordinates and texel positions in 16.16 fixed point, bilinear weights truncated to
 * 8 bits, the result sum(w * texel8) / (65536 * 255) rounded to nearest by the soft float divide.
 */
#ifndef S8GFX_TEX_H
#define S8GFX_TEX_H

#include "s8gfx_sf.h"

enum { S8T_FMT_RGBA8 = 0, S8T_FMT_BGRA8 = 1, S8T_FMT_R8 = 2 };
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
	return fmt == S8T_FMT_R8 ? 1 : 4;
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
 * Decode a texture state record. Recognised: low 16 bits of word 0 = 0x050a (both the texture record 1b44050a and the render target record 1fe3050a carry it).
 *   word2          = VA >> 2                                         [verified: 0x40001440 -> 0x100005100]
 *   word1 bits 0-10 = 0x800 - (0x800 >> k) for a (square) size of 2^k  [hypothesis from two points: 4x4 -> 0x600, 64x64 -> 0x7e0]
 *   word0 >> 16    = 0x1b44 -> RGBA8 2D (only value seen); anything else is taken as RGBA8 too, known = 0
 * Returns 0 when the record is recognised, -1 otherwise (d is still filled with defaults).
 */
static inline int
s8t_desc_decode(struct s8t_desc * d, const uint32_t w[6])
{
	uint32_t m = w[1] & 0x7ff;
	uint32_t k = 0;
	uint32_t size = 0;

	d->va = (uint64_t)w[2] << 2;
	d->fmt = S8T_FMT_RGBA8;
	d->layout = S8T_LAYOUT_MORTON;
	d->levels = 1;
	d->known = 0;
	if ((w[0] & 0xffff) != 0x050a || m == 0) {
		d->width = d->height = 0;
		d->pitch = 0;
		return -1;
	}
	/* m = 0x800 - 0x800 / size: the lowest set bit is at 11 - k */
	while (!((m >> k) & 1)) {
		k++;
	}
	size = 1u << (11 - k);
	d->width = d->height = size;
	d->pitch = size * 4;
	d->known = ((w[0] >> 16) == 0x1b44);
	return 0;
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
	if (d->fmt == S8T_FMT_R8) {
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
