/* isa_hist corpus.bin op[,op...]: walk each program of a corpus file (s8f_len; the walk stops at an unknown length) and list the distinct instruction byte strings of the
 * given opcodes with their counts; "all" lists every opcode histogram. Used to infer fields of the compositor forms. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s8gfx_frag.h"

struct ent { uint8_t b[12]; unsigned n, cnt; };
static struct ent ents[4096];
static int nent;

int main(int argc, char ** argv)
{
	FILE * f = fopen(argv[1], "rb");
	int want[256] = { 0 };
	char kind;
	uint32_t entry, len;
	int all = argc > 2 && strcmp(argv[2], "all") == 0, i;
	int stops[256] = { 0 };
	long hist[256] = { 0 };

	if (argc > 2 && !all) {
		char * s = strtok(argv[2], ",");

		while (s) { want[strtol(s, 0, 16) & 255] = 1; s = strtok(0, ","); }
	}
	while (fread(&kind, 1, 1, f) == 1) {
		uint8_t pad[3];
		static uint8_t buf[8192];
		unsigned pc = 0;

		if (fread(pad, 1, 3, f) != 3 || fread(&entry, 4, 1, f) != 1 || fread(&len, 4, 1, f) != 1 || len > sizeof(buf) || fread(buf, 1, len, f) != len) break;
		while (pc + 2 <= len) {
			unsigned n = s8f_len(buf + pc, len - pc);

			if (n == 0 || pc + n > len) { stops[buf[pc]]++; break; }
			hist[buf[pc]]++;
			if (want[buf[pc]] && n <= 12) {
				for (i = 0; i < nent; i++) if (ents[i].n == n && memcmp(ents[i].b, buf + pc, n) == 0) break;
				if (i == nent && nent < 4096) { memcpy(ents[nent].b, buf + pc, n); ents[nent].n = n; ents[nent].cnt = 0; nent++; }
				if (i < 4096) ents[i].cnt++;
			}
			if (buf[pc] == 0x08 && buf[pc + 1] == 0x01) break;
			pc += n;
		}
	}
	if (all) {
		for (i = 0; i < 256; i++) if (hist[i] || stops[i]) printf("op %02x: %ld walked, %d stops\n", i, hist[i], stops[i]);
		return 0;
	}
	for (int op = 0; op < 256; op++) {
		if (!want[op]) continue;
		for (i = 0; i < nent; i++) {
			if (ents[i].b[0] != op) continue;
			printf("%3u x", ents[i].cnt);
			for (unsigned k = 0; k < ents[i].n; k++) printf(" %02x", ents[i].b[k]);
			printf("\n");
		}
	}
	return 0;
}
