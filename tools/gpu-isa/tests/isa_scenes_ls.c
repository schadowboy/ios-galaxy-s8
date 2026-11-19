/* list the scene programs in scenes.bin: id, length, hex */
#include <stdio.h>
#include <stdint.h>
int main(int argc, char ** argv)
{
	FILE * f = fopen(argv[1], "rb");
	uint32_t id, l;

	while (fread(&id, 4, 1, f) == 1 && fread(&l, 4, 1, f) == 1) {
		uint8_t b[1024];

		if (l > sizeof(b) || fread(b, 1, l, f) != l) break;
		printf("scene %u len %u:", id, l);
		for (unsigned i = 0; i < l; i++) printf(" %02x", b[i]);
		printf("\n");
	}
	return 0;
}
