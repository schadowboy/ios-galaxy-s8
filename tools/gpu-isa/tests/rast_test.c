/* host test: gcc -O0 -ffp-contract=off -o rast_test rast_test.c -lm && ./rast_test */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s8gfx_rast.h"

static float tof(f32b b) { float f; memcpy(&f, &b, 4); return f; }
static f32b tob(float f) { f32b b; memcpy(&b, &f, 4); return b; }

static uint64_t rs = 88172645463325252ULL;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }
static float rfl(float lo, float hi) { return lo + (hi - lo) * ((rnd() & 0xffffff) / 16777216.0f); }

static int hits[64][64];
static float lastv[64][64][2];
static void emit(void * ctx, int px, int py, const f32b * v)
{
	(void)ctx;
	hits[py][px]++;
	lastv[py][px][0] = tof(v[0]);
	lastv[py][px][1] = tof(v[1]);
}

static struct s8r_vtx mk(float x, float y, float w, float v0, float v1)
{
	struct s8r_vtx t;
	memset(&t, 0, sizeof(t));
	t.x = tob(x); t.y = tob(y); t.iw = tob(1.0f / w); t.v[0] = tob(v0); t.v[1] = tob(v1);
	return t;
}

int main(void)
{
	int bad = 0;

	/* 1: the test scene's full-screen triangle: NDC (-1,-1) (3,-1) (-1,3) with viewport 32 + 32 x, 32 - 32 y, varying col = (x*0.5+0.5, y*0.5+0.5) */
	memset(hits, 0, sizeof(hits));
	{
		struct s8r_vtx a = mk(0, 64, 1, 0.0f, 0.0f), b = mk(128, 64, 1, 2.0f, 0.0f), c = mk(0, -64, 1, 0.0f, 2.0f);
		s8r_tri(&a, &b, &c, 2, 0, 0, 64, 64, emit, NULL);
	}
	for (int y = 0; y < 64; y++) {
		for (int x = 0; x < 64; x++) {
			float wantx = (x + 0.5f) / 64.0f, wanty = 1.0f - (y + 0.5f) / 64.0f;
			if (hits[y][x] != 1) { if (bad++ < 10) printf("full-screen: pixel (%d,%d) hit %d times\n", x, y, hits[y][x]); continue; }
			if (fabsf(lastv[y][x][0] - wantx) > 2e-6f || fabsf(lastv[y][x][1] - wanty) > 2e-6f) {
				if (bad++ < 10) printf("full-screen: varying at (%d,%d) = (%g,%g) want (%g,%g)\n", x, y, lastv[y][x][0], lastv[y][x][1], wantx, wanty);
			}
		}
	}
	printf("full-screen triangle: %s\n", bad ? "FAIL" : "all 4096 pixels once, varyings exact");

	/* 2: a quad as two triangles (both windings) covers every pixel in it exactly once; random positions including exact pixel-centre edges */
	int qbad = 0;
	for (int i = 0; i < 4000; i++) {
		float x0 = (float)(int)rfl(0, 30), y0 = (float)(int)rfl(0, 30), x1 = x0 + (float)(int)rfl(1, 33), y1 = y0 + (float)(int)rfl(1, 33);
		if (i & 1) { x0 += 0.5f; y0 += 0.5f; x1 += 0.5f; y1 += 0.5f; }   /* edges exactly through pixel centres */
		if (x1 > 64) x1 = 64;
		if (y1 > 64) y1 = 64;
		memset(hits, 0, sizeof(hits));
		struct s8r_vtx p00 = mk(x0, y0, 1, 0, 0), p10 = mk(x1, y0, 1, 1, 0), p01 = mk(x0, y1, 1, 0, 1), p11 = mk(x1, y1, 1, 1, 1);
		if (i & 2) {
			s8r_tri(&p00, &p10, &p01, 2, 0, 0, 64, 64, emit, NULL);
			s8r_tri(&p10, &p11, &p01, 2, 0, 0, 64, 64, emit, NULL);
		} else {
			s8r_tri(&p00, &p01, &p10, 2, 0, 0, 64, 64, emit, NULL);
			s8r_tri(&p10, &p01, &p11, 2, 0, 0, 64, 64, emit, NULL);
		}
		for (int y = 0; y < 64; y++) {
			for (int x = 0; x < 64; x++) {
				float cx = x + 0.5f, cy = y + 0.5f;
				int inside = cx > x0 && cx < x1 && cy > y0 && cy < y1;
				int edge = (cx == x0 || cx == x1 || cy == y0 || cy == y1) && cx >= x0 && cx <= x1 && cy >= y0 && cy <= y1;
				int want = inside ? 1 : 0;
				if (hits[y][x] > 1 || (!edge && hits[y][x] != want)) { if (qbad++ < 10) printf("quad %d (%g,%g)-(%g,%g): pixel (%d,%d) hit %d want %d\n", i, x0, y0, x1, y1, x, y, hits[y][x], want); }
			}
		}
	}
	bad += qbad;
	printf("shared-edge quads (4000 random): %s\n", qbad ? "FAIL" : "every pixel covered exactly once, none twice");

	/* 3: random triangles vs a float point-in-triangle test (pixels near an edge ignored), perspective-correct varying vs float formula */
	int rbad = 0;
	for (int i = 0; i < 3000; i++) {
		float xs[3], ys[3], ws[3];
		/* positions snapped to 1/256 pixel like the rasteriser (and the hardware) does */
		for (int k = 0; k < 3; k++) { xs[k] = floorf(rfl(-10, 74) * 256.0f + 0.5f) / 256.0f; ys[k] = floorf(rfl(-10, 74) * 256.0f + 0.5f) / 256.0f; ws[k] = rfl(0.5f, 3.0f); }
		struct s8r_vtx a = mk(xs[0], ys[0], ws[0], 0, 0), b = mk(xs[1], ys[1], ws[1], 1, 0), c = mk(xs[2], ys[2], ws[2], 0, 1);
		memset(hits, 0, sizeof(hits));
		s8r_tri(&a, &b, &c, 2, 0, 0, 64, 64, emit, NULL);
		float area = (xs[1] - xs[0]) * (ys[2] - ys[0]) - (ys[1] - ys[0]) * (xs[2] - xs[0]);
		if (fabsf(area) < 1.0f) continue;
		for (int y = 0; y < 64; y++) {
			for (int x = 0; x < 64; x++) {
				float px = x + 0.5f, py = y + 0.5f;
				float e0 = (xs[2] - xs[1]) * (py - ys[1]) - (ys[2] - ys[1]) * (px - xs[1]);
				float e1 = (xs[0] - xs[2]) * (py - ys[2]) - (ys[0] - ys[2]) * (px - xs[2]);
				float e2 = (xs[1] - xs[0]) * (py - ys[0]) - (ys[1] - ys[0]) * (px - xs[0]);
				float sg = area > 0 ? 1.0f : -1.0f;
				float m = fminf(fminf(e0 * sg, e1 * sg), e2 * sg);
				float tol = 0.02f * fmaxf(fabsf(xs[0]) + fabsf(ys[0]) + 1.0f, 1.0f);
				if (fabsf(m) < tol) continue;           /* too close to an edge for a float reference */
				int want = m > 0;
				if (hits[y][x] != want) { if (rbad++ < 10) printf("random tri %d: pixel (%d,%d) hit %d want %d\n", i, x, y, hits[y][x], want); continue; }
				if (want) {
					float l0 = e0 / area / ws[0], l1 = e1 / area / ws[1], l2 = e2 / area / ws[2], s = l0 + l1 + l2;
					float wv0 = (l1 * 1.0f) / s, wv1 = (l2 * 1.0f) / s;
					if (fabsf(lastv[y][x][0] - wv0) > 1e-3f || fabsf(lastv[y][x][1] - wv1) > 1e-3f) { if (rbad++ < 10) printf("random tri %d: varying at (%d,%d) = (%g,%g) want (%g,%g)\n", i, x, y, lastv[y][x][0], lastv[y][x][1], wv0, wv1); }
				}
			}
		}
	}
	bad += rbad;
	printf("random triangles (3000, perspective): %s\n", rbad ? "FAIL" : "coverage and perspective-correct varyings match the float reference");
	return bad != 0;
}
