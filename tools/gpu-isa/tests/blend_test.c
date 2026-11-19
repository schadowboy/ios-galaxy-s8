/* host test: scene 9 (red alpha 0.5 blended over white) = ld_tile + ALU; scenes.bin from capture9 */
#include <stdio.h>
#include <string.h>
#include "s8gfx_frag.h"

static float tof(f32b b) { float f; memcpy(&f, &b, 4); return f; }

int main(int argc, char ** argv)
{
	FILE * f = fopen(argv[1], "rb");
	uint8_t prog[512];
	uint32_t id, l, len = 0;
	int bad = 0;

	while (fread(&id, 4, 1, f) == 1 && fread(&l, 4, 1, f) == 1) {
		uint8_t tmp[512];

		if (fread(tmp, 1, l < 512 ? l : 512, f) == 0) break;
		if (id == 9) { memcpy(prog, tmp, l); len = l; }
	}
	uint32_t dst[3] = { 0xffffffffu, 0xff000000u, 0x80808080u };
	for (int i = 0; i < 3; i++) {
		struct s8f_ctx c;
		float d[4] = { ((dst[i] >> 16) & 255) / 255.0f, ((dst[i] >> 8) & 255) / 255.0f, (dst[i] & 255) / 255.0f, ((dst[i] >> 24) & 255) / 255.0f };
		float want[4] = { d[0] * 0.5f + 0.5f, d[1] * 0.5f, d[2] * 0.5f, d[3] * 0.5f + 0.5f };

		memset(&c, 0, sizeof(c));
		c.tile = dst[i];
		c.has_tile = 1;
		int ok = s8f_run(&c, prog, len);
		printf("dst %08x: ok=%d unknown=0x%02x out %.4f %.4f %.4f %.4f want %.4f %.4f %.4f %.4f\n", dst[i], ok, c.unknown, tof(c.out[0]), tof(c.out[1]), tof(c.out[2]), tof(c.out[3]), want[0], want[1], want[2], want[3]);
		for (int k = 0; k < 4; k++) {
			float diff = tof(c.out[k]) - want[k];
			if (diff > 1e-5f || diff < -1e-5f) bad++;
		}
	}
	printf("%s\n", bad ? "FAIL" : "ld_tile blend reproduces scene 9");
	return bad != 0;
}
