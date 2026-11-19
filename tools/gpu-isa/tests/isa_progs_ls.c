/* list the program of each scene in scenes_raw.bin trimmed at its first 08 01 (walked with s8f_len) */
#include <stdio.h>
#include <string.h>
#include "s8gfx_frag.h"
int main(int argc, char ** argv)
{
	FILE * f = fopen(argv[1], "rb");
	uint32_t id, l;

	while (fread(&id, 4, 1, f) == 1 && fread(&l, 4, 1, f) == 1) {
		uint8_t b[1024];
		unsigned pc = 0;

		if (l > sizeof(b) || fread(b, 1, l, f) != l) break;
		while (pc + 2 <= l) {
			unsigned n = s8f_len(b + pc, l - pc);

			if (n == 0) { printf("scene %u: ?? %02x at %u\n", id, b[pc], pc); break; }
			if (b[pc] == 0x08 && b[pc + 1] == 1) { pc += 4; break; }
			pc += n;
		}
		if (argc > 2 && (unsigned)atoi(argv[2]) != id) continue;
		printf("scene %u len %u:", id, pc);
		for (unsigned i = 0; i < pc; i++) printf(" %02x", b[i]);
		printf("\n");
	}
	return 0;
}
