/*
 * pass_test.c: decode render-pass attachment records from a captured region (final_res_N.bin or a replayed state, tests/replay.py --dump)
 *   gcc -O1 -o pass_test pass_test.c && ./pass_test <region.bin> [region VA hex]
 * Scan rule (analysis/compositor-passes.md section 4):
 *   attachment record = 6 dwords { fmt, w1, VA>>4, 0x00000200, 0, 0 }, VA in [0x1_0000_0000, 0x2_0000_0000), fmt and w1 non-zero.
 *   A record whose VA>>4 is 0x0eeee000 + n is the "shadow" copy of the descriptor (placeholder base 0xeeee0000) and is skipped.
 *   Pass header (3 shapes seen): { 0xf?xxxxxx|fmt16, w1, A, B, C, D } two dwords before the 0x000e0000 0x00000340 marker or earlier.
 * The 4 floats before the record are printed too: in my test passes they are the clear colour (+0x38), in the compositor passes they are matrix/clear leftovers.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float f(uint32_t b) { float x; memcpy(&x, &b, 4); return x; }

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s region.bin [va-hex]\n", argv[0]); return 2; }
	FILE *fp = fopen(argv[1], "rb");
	if (!fp) { perror(argv[1]); return 1; }
	fseek(fp, 0, SEEK_END);
	long len = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	uint32_t *d = malloc(len + 64);
	memset(d, 0, len + 64);
	if (fread(d, 1, len, fp) != (size_t)len) return 1;
	fclose(fp);
	uint64_t base = argc > 2 ? strtoull(argv[2], 0, 16) : 0;
	size_t nd = len / 4;
	int found = 0, shadow = 0;

	for (size_t i = 4; i + 6 <= nd; i++) {
		if (d[i + 3] != 0x200 || d[i] == 0 || d[i + 1] == 0 || d[i + 2] < 0x10000000u || d[i + 2] >= 0x20000000u) {
			if (d[i + 3] == 0x200 && d[i + 2] >= 0x0eee0000u && d[i + 2] < 0x0eef0000u) {
				shadow++;
			}
			continue;
		}
		uint64_t va = (uint64_t)d[i + 2] << 4;
		uint32_t fmt = d[i];
		const char *cls = (d[i + 1] & 0xffff) == 0x07e0 ? "plain (test layout)" : "surface-class";

		found++;
		printf("attachment @ +0x%05zx (VA %#llx): fmt word %08x (code 0x%04x) w1 %08x [%s] target VA 0x%llx\n", i * 4,
		    (unsigned long long)(base + i * 4), fmt, fmt & 0xffff, d[i + 1], cls, (unsigned long long)va);
		printf("   4 floats before: %g %g %g %g   (raw %08x %08x %08x %08x)\n", f(d[i - 4]), f(d[i - 3]), f(d[i - 2]), f(d[i - 1]),
		    d[i - 4], d[i - 3], d[i - 2], d[i - 1]);
		/* the header: nearest dword before with top byte 0xf8..0xff and low 16 bits equal to the format code */
		for (size_t k = i; k-- > (i > 48 ? i - 48 : 0);) {
			if ((d[k] >> 24) >= 0xf8 && (d[k] & 0xffff) == (fmt & 0xffff)) {
				printf("   header @ +0x%05zx: %08x %08x A %08x B %08x C %08x D %08x\n", k * 4, d[k], d[k + 1], d[k + 2], d[k + 3], d[k + 4], d[k + 5]);
				break;
			}
		}
		/* same-VA repeats within 0x80 bytes are the copies of one pass */
		for (size_t k = i + 6; k < i + 0x20 && k + 6 <= nd; k++) {
			if (d[k + 3] == 0x200 && d[k + 2] == d[i + 2] && d[k] == d[i]) {
				printf("   copy of the same attachment at +0x%05zx\n", k * 4);
			}
		}
	}
	printf("%d attachment record(s), %d shadow record(s) (0xeeee0000 placeholder base)\n", found, shadow);
	return 0;
}
