/* run every captured compositor vertex program through the vertex interpreter with synthetic uniforms/memory and report what stops it */
#include <stdio.h>
#include <string.h>
#include "s8gfx_frag.h"

static uint32_t rdany(void * ctx, uint64_t va) { (void)ctx; (void)va; return 0x3f000000u; }   /* 0.5f */

int main(int argc, char ** argv)
{
	FILE * f = fopen(argv[1], "rb");
	char kind;
	uint32_t entry, len;
	int total = 0, okv = 0, unk[256] = { 0 }, idx = 0;

	while (fread(&kind, 1, 1, f) == 1) {
		uint8_t pad[3];
		static uint8_t buf[4096];

		if (fread(pad, 1, 3, f) != 3 || fread(&entry, 4, 1, f) != 1 || fread(&len, 4, 1, f) != 1) break;
		if (len > sizeof(buf) || fread(buf, 1, len, f) != len) break;
		if (kind != 'v') { idx++; continue; }
		struct s8f_ctx c;
		unsigned st = s8v_find_start(buf, len, entry);

		memset(&c, 0, sizeof(c));
		c.rd = rdany;
		c.stride = 16;
		for (int k = 0; k < 128; k += 2) c.uni[k] = 0x3f000000u;
		c.uni[0] = 0x100000; c.uni[2] = 0;
		int ok = s8f_run(&c, buf + st, len - st);
		total++;
		if (ok && (c.expmask & 0xf) == 0xf) okv++; else { unk[c.unknown & 255]++; printf("prog %d: unknown 0x%02x at +%u:", idx, c.unknown, c.pc_fail); for (int q = (int)c.pc_fail - 10; q < (int)c.pc_fail + 14; q++) printf("%s%02x", q == (int)c.pc_fail ? " | " : " ", buf[st + (q < 0 ? 0 : q)]); printf("\n"); }
		idx++;
	}
	printf("%d vertex programs, %d run to completion\n", total, okv);
	for (int k = 0; k < 256; k++) if (unk[k]) printf("  unknown op 0x%02x: %d programs\n", k, unk[k]);
	return 0;
}
