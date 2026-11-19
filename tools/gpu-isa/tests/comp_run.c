/*
 * comp_run corpus_comp.bin [-v]: execute every distinct compositor fragment program (tests/frag_forms.c -o data/corpus_comp.bin: records start at the program start
 * found from the block header / walk, entry offset in the record) with synthetic inputs and report the coverage of the interpreter:
 *   - programs that reach the end instruction / execute a pixel store (st_tile, or the compositor's `31` write, ASSUMED) / stop at an unknown form,
 *   - the opcodes executed (instruction counts) and which of them are ASSUMED (marked *), the opcodes the programs stop at.
 * Synthetic inputs: varyings (0.30, 0.60, 0.25, 1.0), tile word 0xff80c040 (also as 4 distinct per-sample words), uniform words 0.5f, a 16x16 RGBA8 texture, sr[48]/sr[49] = 3/5.
 * The results are not checked (the compositor's semantics are ASSUMED where marked): this runner measures decode coverage and crash-freedom (runs under -fsanitize in r12.sh).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct s8f_ctx;
static void trace_fn(struct s8f_ctx * c, unsigned pc, const unsigned char * b);
static int g_trace_prog = -1, g_cur_prog;
#define S8F_TRACE(c, pc, b) trace_fn(c, pc, b)
#include "s8gfx_frag.h"

/* -t N: instruction trace of program N (halves h0..h23 as f16 hex words) */
static void trace_fn(struct s8f_ctx * c, unsigned pc, const unsigned char * b)
{
	unsigned i;

	if (g_cur_prog != g_trace_prog) return;
	printf("    %04x:", pc);
	for (i = 0; i < 8; i++) printf(" %02x", b[i]);
	printf(" |");
	for (i = 0; i < 24; i++) printf(" %04x", s8f_gh(c, i));
	printf("\n");
}

static int g_const_tex;
static uint32_t texrd(void * ctx, uint64_t va)
{
	(void)ctx;
	if (g_const_tex) return getenv("HH_TEX") ? (uint32_t)strtoul(getenv("HH_TEX"), 0, 16) : 0xffbf4080u;    /* RGBA8 bytes 80 40 bf ff = (0.502, 0.251, 0.749, 1.0) */
	return (uint32_t)(va * 2654435761u) | 0xff000000u;
}
static float tofl(f32b b) { float x; memcpy(&x, &b, 4); return x; }

static const char * g_mode = "";
static int g_const;

int main(int argc, char ** argv)
{
	FILE * f = fopen(argv[1], "rb");
	int verbose = argc > 2 && strcmp(argv[2], "-v") == 0;

	/* -const: constant opaque texel (0.5, 0.25, 0.75, 1.0), transparent black destination, every uniform half = f16 1.0 (opacity 1, colour white): print
	 * program -> output colour (the expected value of a correct compositor shader is the texel, or the layer colour * texel for modulated ones) */
	if (argc > 2 && strcmp(argv[2], "-const") == 0) { g_const = 1; verbose = 1; g_mode = "const"; }
	if (argc > 3 && strcmp(argv[2], "-const") == 0 && strcmp(argv[3], "-t") == 0) g_trace_prog = atoi(argv[4]);
	char kind;
	uint32_t entry, len;
	int idx = 0, total = 0, end_n = 0, store_n = 0, stop_n = 0, clean_n = 0, tex_n = 0;
	unsigned long ex[256] = { 0 }, stopat[256] = { 0 };
	uint32_t assumed_any[8] = { 0 };

	while (fread(&kind, 1, 1, f) == 1) {
		uint8_t pad[3];
		static uint8_t buf[8192];
		static struct s8f_ctx c;
		static f32b vv[8] = { 0x3e99999au, 0x3f19999au, 0x3e800000u, 0x3f800000u, 0x3f000000u, 0x3f000000u, 0x3f000000u, 0x3f800000u };
		int i, any_assumed = 0;

		if (fread(pad, 1, 3, f) != 3 || fread(&entry, 4, 1, f) != 1 || fread(&len, 4, 1, f) != 1 || len > sizeof(buf) || fread(buf, 1, len, f) != len) break;
		if (kind != 'f') { idx++; continue; }
		memset(&c, 0, sizeof(c));
		c.varying = vv;
		g_const_tex = g_const;
		c.tile = g_const ? 0u : 0xff80c040u;
		c.has_tile = 1;
		for (i = 0; i < 128; i += 2) c.uni[i] = g_const ? 0x3c003c00u : 0x3f000000u;
		if (g_const) for (i = 1; i < 128; i += 2) c.uni[i] = 0x3c003c00u;
		c.sr[48] = 3;
		c.sr[49] = 5;
		c.ntex = 1;
		s8t_desc_make(&c.texd[0], 0x1000, 16, 16, S8T_FMT_RGBA8, S8T_LAYOUT_MORTON, 0);
		c.texs[0].mag_linear = c.texs[0].min_linear = 1;
		c.trd = texrd;
		total++;
		g_cur_prog = idx;
		if (idx == g_trace_prog && getenv("HH_TILE")) c.tile = (uint32_t)strtoul(getenv("HH_TILE"), 0, 16);
		{
			unsigned st0 = (idx == g_trace_prog && getenv("HH_START")) ? (unsigned)strtoul(getenv("HH_START"), 0, 16) : 0;
			int ok = s8f_run(&c, buf + st0, len - st0);

			if (c.pc_end) end_n++;
			if (ok && c.stored) store_n++;
			if (!c.pc_end) { stop_n++; stopat[c.unknown]++; }
			if (c.ntex_missing || c.hit[0x21]) tex_n++;
			for (i = 0; i < 8; i++) { if (c.assumed[i]) any_assumed = 1; assumed_any[i] |= c.assumed[i]; }
			if (c.pc_end && !any_assumed) clean_n++;
			for (i = 0; i < 256; i++) ex[i] += c.hit[i];
			if (verbose || !c.pc_end) {
				printf("program %3d (%s, %u B): %s%s%s", idx, pad[0] == 1 ? "block" : "walk ", len, c.pc_end ? "end" : "STOP", ok && c.stored ? " +store" : "", any_assumed ? " (ASSUMED forms)" : " (decoded forms only)");
				if (c.pc_end && ok && c.stored) printf("  -> rgba %.4f %.4f %.4f %.4f%s", tofl(c.out[0]), tofl(c.out[1]), tofl(c.out[2]), tofl(c.out[3]), c.ntex_missing ? " (texture missing)" : "");
				if (c.pc_end && c.hit[0x21]) printf(" [%u samples, tex hits %u]", c.hit[0x21], c.hit[0x21]);
				if (!c.pc_end) printf(" unknown 0x%02x at +0x%x:", c.unknown, c.pc_fail);
				if (!c.pc_end) for (i = 0; i < 8; i++) printf(" %02x", buf[c.pc_fail + i]);
				printf("\n");
			}
		}
		idx++;
	}
	printf("\n%d compositor fragment programs: %d reach the end instruction, %d execute a pixel store, %d stop at an unknown form, %d run with decoded (proven) forms only, %d sample textures\n",
	    total, end_n, store_n, stop_n, clean_n, tex_n);
	printf("opcodes executed (count; * = ASSUMED semantics somewhere in the corpus):\n");
	for (int op = 0; op < 256; op++) {
		if (ex[op]) printf("  %02x%s %lu\n", op, (assumed_any[op >> 5] >> (op & 31)) & 1 ? "*" : " ", ex[op] - stopat[op]);
	}
	printf("stops by opcode:");
	for (int op = 0; op < 256; op++) if (stopat[op]) printf(" %02x x%lu", op, stopat[op]);
	printf("\n");
	return 0;
}
