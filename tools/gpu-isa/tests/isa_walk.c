/* structural check: walk every captured program with s8f_len() and report where the walk does not land on the 08 01 end instruction.
 * usage: isa_walk corpus.bin v|f   (prog start = s8v_find_start for v; for f: the earliest offset in [0, entry] whose walk reaches 08 01 at the end) */
#include <stdio.h>
#include <string.h>
#include "s8gfx_frag.h"

static int walk(const uint8_t * m, unsigned len, unsigned q, unsigned * endp, unsigned * failp)
{
	unsigned pc = q;

	while (pc + 2 <= len) {
		unsigned n = s8f_len(m + pc, len - pc);

		if (n == 0) { *failp = pc; return 0; }
		if (m[pc] == 0x08 && m[pc + 1] == 0x01) { *endp = pc + 4; return 1; }
		pc += n;
	}
	*failp = pc;
	return 0;
}

int main(int argc, char ** argv)
{
	FILE * f = fopen(argv[1], "rb");
	char kind, want = argv[2][0];
	uint32_t entry, len;
	int idx = 0, ok = 0, tot = 0;

	while (fread(&kind, 1, 1, f) == 1) {
		uint8_t pad[3];
		static uint8_t buf[8192];

		if (fread(pad, 1, 3, f) != 3 || fread(&entry, 4, 1, f) != 1 || fread(&len, 4, 1, f) != 1) break;
		if (len > sizeof(buf) || fread(buf, 1, len, f) != len) break;
		if (kind == want) {
			unsigned q, st = 0, end = 0, fail = 0;
			int good = 0;

			if (want == 'v') {
				st = s8v_find_start(buf, len, entry);
				good = walk(buf, len, st, &end, &fail);
			} else {
				for (q = 0; q <= entry && !good; q++) {
					good = walk(buf, len, q, &end, &fail);
					if (good && q + 8 < entry && end <= entry) good = 0;   /* an earlier program's walk that ends before the entry pointer */
					if (good) st = q;
				}
			}
			tot++;
			if (good) ok++;
			printf("%s %d: entry %u start %u %s end %u fail %u (op %02x)\n", want == 'v' ? "vert" : "frag", idx, entry, st, good ? "OK " : "BAD", end, fail, buf[fail]);
		}
		idx++;
	}
	printf("%d programs, %d walk to the end instruction\n", tot, ok);
	return 0;
}
