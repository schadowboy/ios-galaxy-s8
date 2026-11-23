/*
 * Triangle rasteriser of the kernel-side AGX executor. Integer coverage (28.8 fixed point subpixel positions, top-left fill rule), perspective-correct
 * varyings in the integer-only soft-float (s8gfx_sf.h). No libc: builds into the kernel and into the host test (scratch/agx/rast_test.c).
 *
 * Input vertices are already transformed to framebuffer coordinates: x, y in pixels (float bits), iw = 1 / w (float bits), z unused here.
 * s8r_tri calls emit(ctx, px, py, varyings) for every covered pixel centre, varyings interpolated for that pixel.
 */
#ifndef S8GFX_RAST_H
#define S8GFX_RAST_H

#include "s8gfx_sf.h"

#define S8R_MAXV 8              /* varying components per vertex */

struct s8r_vtx {
	f32b x, y;              /* framebuffer position in pixels */
	f32b iw;                /* 1 / w */
	f32b v[S8R_MAXV];       /* varyings (not yet divided by w) */
};

typedef void (*s8r_emit)(void * ctx, int px, int py, const f32b * varying);

/* native-FP windows (s8gfx_nfp.h): the kernel defines these before including this header to run the pixel work with the hardware FPU in short
 * windows; on the host and by default they do nothing. */
#ifndef S8R_FP_BEGIN
#define S8R_FP_DECL
#define S8R_FP_BEGIN()  ((void)0)
#define S8R_FP_END()    ((void)0)
#define S8R_FP_CHUNK    128
#endif

/* scissor: pixels [x0,x1) x [y0,y1) only */
static inline void
s8r_tri(const struct s8r_vtx * a, const struct s8r_vtx * b, const struct s8r_vtx * c, int nv, int x0, int y0, int x1, int y1, s8r_emit emit, void * ctx)
{
	int32_t ax = sf_to_fixed(a->x, 8), ay = sf_to_fixed(a->y, 8);
	int32_t bx = sf_to_fixed(b->x, 8), by = sf_to_fixed(b->y, 8);
	int32_t cx = sf_to_fixed(c->x, 8), cy = sf_to_fixed(c->y, 8);
	int64_t area = (int64_t)(bx - ax) * (cy - ay) - (int64_t)(by - ay) * (cx - ax);   /* twice the signed area, 16 fractional bits */
	int32_t minx, maxx, miny, maxy;
	f32b farea;
	int swap = 0;
	int32_t px, py;
	S8R_FP_DECL

	if (area == 0) {
		return;
	}
	if (area < 0) {                 /* back-facing / clockwise: rasterise anyway (no culling here), by swapping b and c */
		int32_t t;

		t = bx; bx = cx; cx = t;
		t = by; by = cy; cy = t;
		area = -area;
		swap = 1;
	}
	{
		const struct s8r_vtx * vb = swap ? c : b;
		const struct s8r_vtx * vc = swap ? b : c;
		minx = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx);
		maxx = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
		miny = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy);
		maxy = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);
		minx = (minx >> 8) - 1;
		maxx = (maxx >> 8) + 1;
		miny = (miny >> 8) - 1;
		maxy = (maxy >> 8) + 1;
		if (minx < x0) minx = x0;
		if (miny < y0) miny = y0;
		if (maxx >= x1) maxx = x1 - 1;
		if (maxy >= y1) maxy = y1 - 1;
		farea = sf_from_i64(area);

		for (py = miny; py <= maxy; py++) {
			S8R_FP_BEGIN();
			for (px = minx; px <= maxx; px++) {
				if (px != minx && ((px - minx) % S8R_FP_CHUNK) == 0) {
					S8R_FP_END();
					S8R_FP_BEGIN();
				}
				int32_t sx = px * 256 + 128, sy = py * 256 + 128;       /* pixel centre */
				/* edge functions: e0 opposite a (edge b->c), e1 opposite b (edge c->a), e2 opposite c (edge a->b) */
				int64_t e0 = (int64_t)(cx - bx) * (sy - by) - (int64_t)(cy - by) * (sx - bx);
				int64_t e1 = (int64_t)(ax - cx) * (sy - cy) - (int64_t)(ay - cy) * (sx - cx);
				int64_t e2 = (int64_t)(bx - ax) * (sy - ay) - (int64_t)(by - ay) * (sx - ax);
				/* top-left rule: an edge that is not top/left excludes the samples exactly on it */
				int tl0 = (cy < by) || (cy == by && cx < bx);       /* "left" edges go up in a y-down system with counter-clockwise order */
				int tl1 = (ay < cy) || (ay == cy && ax < cx);
				int tl2 = (by < ay) || (by == ay && bx < ax);
				f32b w0, w1, w2, sum, out[S8R_MAXV] = { 0, 0, 0, 0, 0, 0, 0, 0 };

				if (e0 < 0 || e1 < 0 || e2 < 0) {
					continue;
				}
				if ((e0 == 0 && !tl0) || (e1 == 0 && !tl1) || (e2 == 0 && !tl2)) {
					continue;
				}
				/* perspective-correct barycentrics: weights e_i * iw_i, normalised by their sum */
				w0 = sf_mul(sf_from_i64(e0), a->iw);
				w1 = sf_mul(sf_from_i64(e1), vb->iw);
				w2 = sf_mul(sf_from_i64(e2), vc->iw);
				sum = sf_add(sf_add(w0, w1), w2);
				for (int k = 0; k < nv && k < S8R_MAXV; k++) {
					f32b num = sf_fma(w2, vc->v[k], sf_fma(w1, vb->v[k], sf_mul(w0, a->v[k])));

					out[k] = sf_div(num, sum);
				}
				emit(ctx, px, py, out);
			}
			S8R_FP_END();
		}
	}
	(void)farea;
}

#endif /* S8GFX_RAST_H */
