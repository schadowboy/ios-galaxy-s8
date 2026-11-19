/* hh_start <snapshot dir> <entry va hex>: find the region, run s8f_start_image on the window (block = -1) and report the start and kind */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s8gfx_frag.h"

int main(int argc, char ** argv)
{
	char path[512], line[256];
	FILE * f;
	unsigned long long va = strtoull(argv[2], 0, 16);
	unsigned long long block = argc > 3 ? strtoull(argv[3], 0, 16) : 0;

	snprintf(path, sizeof(path), "%s/regions.txt", argv[1]);
	f = fopen(path, "r");
	while (fgets(line, sizeof(line), f)) {
		char name[64];
		unsigned long long rva, len;

		if (sscanf(line, "%63s %llx %llx", name, &rva, &len) != 3) continue;
		if (va >= rva && va < rva + len) {
			unsigned char * m = (unsigned char *)calloc(1, len);
			unsigned st = 0;
			int how;
			FILE * g;

			snprintf(path, sizeof(path), "%s/%s.bin", argv[1], name);
			g = fopen(path, "rb");
			if (fread(m, 1, len, g) < 1) return 2;
			how = s8f_start_image(m, (unsigned)len, block ? (unsigned)(block - rva) : (unsigned)-1, (unsigned)(va - rva), &st);
			printf("region %s: start_image kind %d start %+d from entry (offset 0x%x)\n", name, how, (int)st - (int)(va - rva), st);
			return 0;
		}
	}
	printf("no region\n");
	return 1;
}
