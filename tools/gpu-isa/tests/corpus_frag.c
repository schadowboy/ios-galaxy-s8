/* run every captured compositor FRAGMENT program through the fragment interpreter with synthetic varyings and a tile pixel; report what stops it.
 * The entry pointer of the captured records lands inside the program (the windows start 32 bytes before it), so the real start is searched: the earliest
 * offset whose instruction-length walk (s8f_len) reaches the 08 01 end instruction at or after the entry pointer.
 * Metrics: `run to st_tile` (the original metric: a pixel store executed: st_tile or the compositor's end-of-shader write 0x31, ASSUMED) and `run to end`. */
#include <stdio.h>
#include <string.h>
#include "s8gfx_frag.h"

static int walk(const uint8_t * m, unsigned len, unsigned q, unsigned * endp)
{
	unsigned pc = q;

	while (pc + 2 <= len) {
		unsigned n = s8f_len(m + pc, len - pc);

		if (n == 0 || pc + n > len) return 0;
		if (m[pc] == 0x08 && m[pc + 1] == 0x01) { *endp = pc + 4; return 1; }
		pc += n;
	}
	return 0;
}

int main(int argc, char ** argv)
{
	FILE * f = fopen(argv[1], "rb");
	char kind;
	uint32_t entry, len;
	int total = 0, ok_n = 0, end_n = 0, idx = 0;

	while (fread(&kind, 1, 1, f) == 1) {
		uint8_t pad[3];
		static uint8_t buf[8192];

		if (fread(pad, 1, 3, f) != 3 || fread(&entry, 4, 1, f) != 1 || fread(&len, 4, 1, f) != 1) break;
		if (len > sizeof(buf) || fread(buf, 1, len, f) != len) break;
		if (kind != 'f') { idx++; continue; }
		struct s8f_ctx c;
		f32b vv[8] = { 0x3f000000u, 0x3f000000u, 0x3f000000u, 0x3f800000u, 0, 0, 0, 0 };
		unsigned start = entry, end = 0, q;

		for (q = 0; q <= entry; q++) {
			if (walk(buf, len, q, &end) && end > entry) { start = q; break; }
		}
		memset(&c, 0, sizeof(c));
		c.varying = vv;
		c.tile = 0xff808080u;
		c.has_tile = 1;
		int ok = s8f_run(&c, buf + start, len - start);
		total++;
		if (c.pc_end) end_n++;
		if (ok && c.stored) {
			ok_n++;
			printf("frag %d: runs to a pixel store (start %u, end %u)%s\n", idx, start, start + c.pc_end, c.assumed[0] | c.assumed[1] | c.assumed[2] | c.assumed[3] | c.assumed[4] | c.assumed[5] | c.assumed[6] | c.assumed[7] ? " with ASSUMED forms" : "");
		} else if (c.pc_end) {
			printf("frag %d: runs to the end instruction, no pixel store (empty program, start %u)\n", idx, start);
		} else {
			printf("frag %d: stops, unknown 0x%02x at +%u:", idx, c.unknown, c.pc_fail);
			for (int q2 = (int)c.pc_fail - 8; q2 < (int)c.pc_fail + 16; q2++) printf("%s%02x", q2 == (int)c.pc_fail ? " | " : " ", buf[start + (q2 < 0 ? 0 : q2)]);
			printf("\n");
		}
		idx++;
	}
	printf("%d fragment programs, %d run to st_tile (pixel store), %d reach the end instruction\n", total, ok_n, end_n);
	return 0;
}
