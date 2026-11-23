/* hh_walk <snapshot dir> <start va hex> <entry va hex>: length-walk from start, report the first unknown length and the shaders' ends (08 01) passed */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s8gfx_frag.h"

int main(int argc, char ** argv)
{
	char path[512], line[256];
	FILE * f;
	unsigned long long sva = strtoull(argv[2], 0, 16), eva = strtoull(argv[3], 0, 16);

	setbuf(stdout, 0);
	snprintf(path, sizeof(path), "%s/regions.txt", argv[1]);
	f = fopen(path, "r");
	while (fgets(line, sizeof(line), f)) {
		char name[64];
		unsigned long long rva, len;

		if (sscanf(line, "%63s %llx %llx", name, &rva, &len) != 3) continue;
		if (sva >= rva && sva < rva + len) {
			unsigned char * m = (unsigned char *)calloc(1, len);
			unsigned pc = (unsigned)(sva - rva), ent = (unsigned)(eva - rva);
			FILE * g;

			snprintf(path, sizeof(path), "%s/%s.bin", argv[1], name);
			g = fopen(path, "rb");
			if (fread(m, 1, len, g) < 1) return 2;
			while (pc + 16 <= len) {
				unsigned n = s8f_len(m + pc, (unsigned)len - pc);

				if (getenv("HH_V") && (int)pc - (int)ent > -64) printf("  %+d: %02x %02x %02x %02x len %u\n", (int)pc - (int)ent, m[pc], m[pc+1], m[pc+2], m[pc+3], n);
				if (!n) { printf("unknown length at va %llx (+%u from start, entry %+d): %02x %02x %02x %02x %02x %02x %02x %02x\n", rva + pc, (unsigned)(rva + pc - sva), (int)pc - (int)ent, m[pc], m[pc+1], m[pc+2], m[pc+3], m[pc+4], m[pc+5], m[pc+6], m[pc+7]); return 0; }
				if (m[pc] == 8 && m[pc + 1] == 1) { printf("end 08 01 at va %llx (entry %+d)\n", rva + pc, (int)pc - (int)ent); pc += 4; while (m[pc] == 8 && m[pc+1] == 0) pc += 2; if (pc > ent + 0x400) return 0; continue; }
				pc += n;
			}
			return 0;
		}
	}
	return 1;
}
