/* isa_dis corpus.bin [index]: print each program record (or one) one instruction per line with its offset; the entry pointer is marked with * */
#include <stdio.h>
#include <stdlib.h>
#include "s8gfx_frag.h"

int main(int argc, char ** argv)
{
	FILE * f = fopen(argv[1], "rb");
	char kind;
	uint32_t entry, len;
	int idx = 0, want = argc > 2 ? atoi(argv[2]) : -1;

	while (fread(&kind, 1, 1, f) == 1) {
		uint8_t pad[3];
		static uint8_t buf[8192];
		unsigned pc = 0;

		if (fread(pad, 1, 3, f) != 3 || fread(&entry, 4, 1, f) != 1 || fread(&len, 4, 1, f) != 1 || len > sizeof(buf) || fread(buf, 1, len, f) != len) break;
		if (want < 0 || want == idx) {
			printf("== program %d (%s) entry +0x%x\n", idx, pad[0] == 1 ? "block" : "walk", entry);
			while (pc + 2 <= len) {
				unsigned n = s8f_len(buf + pc, len - pc);

				if (!n || pc + n > len) { printf("  %04x: ?? %02x %02x %02x %02x %02x %02x\n", pc, buf[pc], buf[pc + 1], buf[pc + 2], buf[pc + 3], buf[pc + 4], buf[pc + 5]); break; }
				printf("  %04x%c", pc, pc == entry ? '*' : ' ');
				for (unsigned i = 0; i < n; i++) printf(" %02x", buf[pc + i]);
				printf("\n");
				if (buf[pc] == 0x08 && buf[pc + 1] == 1) {
					if (argc <= 3 || pc > entry + 8) break;          /* 3rd argument: keep listing the shaders that follow in the image up to the one containing the entry + 8 */
					pc += 4;
					while (pc + 2 <= len && buf[pc] == 0x08 && buf[pc + 1] == 0) pc += 2;
					printf("  ---- next shader\n");
					continue;
				}
				pc += n;
			}
		}
		idx++;
	}
	return 0;
}
