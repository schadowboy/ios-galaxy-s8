/* hexdump every program of kind argv[2] (f or v) of a corpus file, from entry to the 08 01 end (or 128 bytes) */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
int main(int argc, char ** argv)
{
	FILE * f = fopen(argv[1], "rb");
	char kind, want = argc > 2 ? argv[2][0] : 'f';
	uint32_t entry, len;
	int idx = 0;

	while (fread(&kind, 1, 1, f) == 1) {
		uint8_t pad[3];
		static uint8_t buf[8192];

		if (fread(pad, 1, 3, f) != 3 || fread(&entry, 4, 1, f) != 1 || fread(&len, 4, 1, f) != 1) break;
		if (len > sizeof(buf) || fread(buf, 1, len, f) != len) break;
		if (kind == want) {
			printf("== %d entry %u len %u\n", idx, entry, len);
			for (unsigned i = (argc > 3 ? 0 : entry); i < len && i < entry + 600; i++) {
				printf("%02x ", buf[i]);
				if (((i - entry) & 15) == 15) printf("\n");
			}
			printf("\n");
		}
		idx++;
	}
	return 0;
}
