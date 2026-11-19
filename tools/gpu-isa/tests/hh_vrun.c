/* hh_vrun <snapshot dir> <entry va hex> <back>: run the vertex interpreter (fetch mode, like s8v_find_start) from entry - back and report where it stops */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s8gfx_frag.h"

int main(int argc, char ** argv)
{
	char path[512], line[256];
	FILE * f;
	unsigned long long va = strtoull(argv[2], 0, 16);
	unsigned back = (unsigned)strtoul(argv[3], 0, 0);

	snprintf(path, sizeof(path), "%s/regions.txt", argv[1]);
	f = fopen(path, "r");
	while (fgets(line, sizeof(line), f)) {
		char name[64];
		unsigned long long rva, len;

		if (sscanf(line, "%63s %llx %llx", name, &rva, &len) != 3) continue;
		if (va >= rva && va < rva + len) {
			unsigned char * m = (unsigned char *)calloc(1, len);
			unsigned off = (unsigned)(va - rva), q = off - back, i;
			struct s8f_ctx c;
			f32b zero[8] = { 0 };
			FILE * g;

			snprintf(path, sizeof(path), "%s/%s.bin", argv[1], name);
			g = fopen(path, "rb");
			if (fread(m, 1, len, g) < 1) return 2;
			memset(&c, 0, sizeof(c));
			c.fetch = zero; c.nfetch = 4; c.varying = zero;
			if (argc > 4) { c.fetch = 0; c.nfetch = 0; c.rd = s8f_rd_zero; c.stride = 16; }
			i = (unsigned)s8f_run(&c, m + q, (unsigned)len - q);
			printf("run from -%u: ok %u expmask %x unknown %02x at +%u (abs %x) pc_end %u\n", back, i, c.expmask, c.unknown, c.pc_fail, q + c.pc_fail, c.pc_end);
			if (c.unknown) { for (i = 0; i < 10; i++) printf(" %02x", m[q + c.pc_fail + i]); printf("\n"); }
			return 0;
		}
	}
	return 1;
}
