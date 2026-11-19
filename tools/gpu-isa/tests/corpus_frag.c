/* run every captured compositor FRAGMENT program through the fragment interpreter with synthetic varyings and a tile pixel; report what stops it */
#include <stdio.h>
#include <string.h>
#include "s8gfx_frag.h"

int main(int argc, char ** argv)
{
	FILE * f = fopen(argv[1], "rb");
	char kind;
	uint32_t entry, len;
	int total = 0, ok_n = 0, idx = 0;

	while (fread(&kind, 1, 1, f) == 1) {
		uint8_t pad[3];
		static uint8_t buf[8192];

		if (fread(pad, 1, 3, f) != 3 || fread(&entry, 4, 1, f) != 1 || fread(&len, 4, 1, f) != 1) break;
		if (len > sizeof(buf) || fread(buf, 1, len, f) != len) break;
		if (kind != 'f') { idx++; continue; }
		struct s8f_ctx c;
		f32b vv[8] = { 0x3f000000u, 0x3f000000u, 0x3f000000u, 0x3f800000u, 0, 0, 0, 0 };

		memset(&c, 0, sizeof(c));
		c.varying = vv;
		c.tile = 0xff808080u;
		c.has_tile = 1;
		int ok = s8f_run(&c, buf + entry, len - entry);
		total++;
		if (ok && c.stored) {
			ok_n++;
			printf("frag %d: runs to st_tile (entry %u)\n", idx, entry);
		} else {
			printf("frag %d: stops, unknown 0x%02x at +%u:", idx, c.unknown, c.pc_fail);
			for (int q = (int)c.pc_fail - 8; q < (int)c.pc_fail + 16; q++) printf("%s%02x", q == (int)c.pc_fail ? " | " : " ", buf[entry + (q < 0 ? 0 : q)]);
			printf("\n");
		}
		idx++;
	}
	printf("%d fragment programs, %d run to st_tile\n", total, ok_n);
	return 0;
}
