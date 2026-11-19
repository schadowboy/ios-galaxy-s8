/* host test: the pack/unpack round-trip program g2 captured from the phone (iter, pack 45, unpack 05, st_tile): output == colour quantised to 8 bits */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "s8gfx_frag.h"

static float tof(f32b b) { float f; memcpy(&f, &b, 4); return f; }
static f32b tob(float f) { f32b b; memcpy(&b, &f, 4); return b; }

static const uint8_t g2[] = {
	0x51, 0x02, 0x01, 0x80, 0x00, 0x00, 0x10, 0x00,
	0x45, 0x02, 0x00, 0x11, 0x00, 0xc0, 0xf2, 0x00,
	0x05, 0x02, 0x00, 0x11, 0x00, 0xc0, 0xf2, 0x00,
	0x38, 0x00, 0x48, 0x00, 0xc2, 0x00, 0x48, 0x0c, 0x00, 0x00,
	0x09, 0x02, 0x00, 0x00, 0x00, 0x00, 0xf2, 0xff,
	0x08, 0x01, 0x00, 0x00 };

int main(void)
{
	int bad = 0;
	float cols[5][4] = { { 0.3f, 0.6f, 0.9f, 0.2f }, { 0.0f, 1.0f, 0.5f, 0.25f }, { 0.123f, 0.456f, 0.789f, 1.0f }, { 0.5f, 0.5f, 0.5f, 0.5f }, { 0.004f, 0.996f, 0.333f, 0.667f } };

	for (int i = 0; i < 5; i++) {
		struct s8f_ctx c;
		f32b vv[4] = { tob(cols[i][0]), tob(cols[i][1]), tob(cols[i][2]), tob(cols[i][3]) };

		memset(&c, 0, sizeof(c));
		c.varying = vv;
		int ok = s8f_run(&c, g2, sizeof(g2));
		printf("col %.3f %.3f %.3f %.3f: ok=%d unknown=0x%02x out", cols[i][0], cols[i][1], cols[i][2], cols[i][3], ok, c.unknown);
		for (int k = 0; k < 4; k++) {
			float want = (float)(floor((double)cols[i][k] * 255.0 + 0.5) / 255.0);

			printf(" %.5f(d%.1e)", tof(c.out[k]), tof(c.out[k]) - want);
			if (!ok || fabsf(tof(c.out[k]) - want) > 1e-5f) bad++;
		}
		printf("\n");
	}
	printf("%s\n", bad ? "FAIL" : "pack/unpack round trip = 8-bit quantisation");
	return bad != 0;
}
