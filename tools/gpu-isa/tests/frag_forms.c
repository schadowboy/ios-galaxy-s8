/*
 * frag_forms <snapshot dir>...: collect every fragment program the compositor draws use (core/s8gfx_stream.h scan of every pass of every given
 * snapshot, tests/replay_dump.py), merge the program bytes of all snapshots (a byte that is zero in one capture is taken from another), then
 *   - print the first 64 bytes of each distinct program split into instructions (mnemonic guesses, '??' = form not known),
 *   - count instruction opcodes over all programs and mark each as LEN-KNOWN (core s8f_len), LEN-ASSUMED (this file) or UNKNOWN (walk stops),
 *   - list the sampled textures (type-0x17 pushes): record words and texel VA (for analysis/compositor-frag-forms.md).
 * Output is plain text; tests/frag_forms.sh runs it on capture17 + capture18.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s8gfx_frag.h"
#include "s8gfx_stream.h"

struct reg { char name[32]; uint64_t va, len; uint8_t * m; };
struct mem { struct reg r[256]; int n; };
static struct mem mems[12], merged;
static struct mem * cur;

static struct reg * findreg(struct mem * M, uint64_t va)
{
	int i;

	for (i = 0; i < M->n; i++) if (M->r[i].va && va >= M->r[i].va && va < M->r[i].va + M->r[i].len) return &M->r[i];
	return 0;
}

static int rd(void * ctx, uint64_t va, uint32_t * out)
{
	struct reg * r = findreg(cur, va);

	(void)ctx;
	if (!r || va + 4 > r->va + r->len) return 0;
	memcpy(out, r->m + (va - r->va), 4);
	return 1;
}

static uint8_t * shm_of(struct mem * M, long * len)
{
	int i;

	for (i = 0; i < M->n; i++) if (strcmp(M->r[i].name, "shmem_100") == 0) { *len = (long)M->r[i].len; return M->r[i].m; }
	return 0;
}

static int load_dir(const char * dir, struct mem * M)
{
	char path[512], line[256];
	FILE * f;

	snprintf(path, sizeof(path), "%s/regions.txt", dir);
	f = fopen(path, "r");
	if (!f) return 0;
	while (fgets(line, sizeof(line), f) && M->n < 256) {
		struct reg * r = &M->r[M->n];
		unsigned long long va, len;
		FILE * g;

		if (sscanf(line, "%31s %llx %llx", r->name, &va, &len) != 3) continue;
		r->va = va; r->len = len;
		snprintf(path, sizeof(path), "%s/%s.bin", dir, r->name);
		g = fopen(path, "rb");
		if (!g) continue;
		r->m = (uint8_t *)calloc(1, len);
		if (fread(r->m, 1, len, g) < 1) {}
		fclose(g);
		M->n++;
	}
	fclose(f);
	return 1;
}

static void merge_in(struct mem * dst, struct mem * src)
{
	int i, j;

	for (i = 0; i < src->n; i++) {
		struct reg * s = &src->r[i];
		struct reg * d = 0;

		if (s->va == 0) continue;
		for (j = 0; j < dst->n; j++) if (dst->r[j].va == s->va) d = &dst->r[j];
		if (!d) {
			d = &dst->r[dst->n++];
			*d = *s;
			d->m = (uint8_t *)calloc(1, s->len);
			memcpy(d->m, s->m, s->len);
			continue;
		}
		if (d->len == s->len) for (j = 0; (uint64_t)j < s->len; j++) if (d->m[j] == 0) d->m[j] = s->m[j];
	}
}

/* instruction names (best guess, see analysis/a11-isa-forms.md and compositor-frag-forms.md) */
static const char * opname(uint8_t op)
{
	switch (op) {
	case 0x08: return "end"; case 0x38: return "wait"; case 0x48: return "wait_pix"; case 0x58: return "signal_pix";
	case 0x09: case 0x89: return "st_tile"; case 0x79: case 0xf9: return "ld_tile"; case 0x51: return "iter"; case 0x21: return "texture_sample";
	case 0x4a: return "fmul"; case 0x8a: return "fadd"; case 0xca: return "ffma"; case 0x0a: return "funop";
	case 0x52: case 0xd2: return "mov_imm"; case 0x02: case 0x42: case 0x82: case 0xa2: case 0xc2: case 0xe2: return "mov/get_sr";
	case 0x0e: case 0x4e: return "iadd"; case 0x8e: case 0xce: return "imad16?"; case 0x1e: case 0x5e: case 0x9e: return "imadd";
	case 0x6e: return "bfeil"; case 0x2e: return "bfi"; case 0x7e: return "bitop"; case 0xbe: return "convert"; case 0x3e: case 0xfe: case 0xde: return "convert?";
	case 0x05: case 0x45: case 0x85: case 0xc5: return "device_ld/st"; case 0xc6: case 0xf6: case 0x76: return "f16_ffma"; case 0x86: case 0x46: return "f16_fadd?";
	case 0xfa: case 0x7a: return "f16_alu?"; case 0x31: return "pixel_write?"; case 0x16: return "op16?"; case 0x26: return "op26?"; case 0x12: case 0x92: return "mov_imm16";
	case 0x23: return "op23?"; case 0x20: return "literal"; case 0x1a: case 0x5a: return "fmul_long";
	default: return "??";
	}
}

/* 0 = not known, 1 = core s8f_len, 2 = assumed here */
static unsigned ilen(const uint8_t * m, unsigned avail, int * kind)
{
	unsigned n = s8f_len(m, avail);

	if (n) { *kind = 1; return n; }
	if (m[0] == 0xf9 || m[0] == 0xf6 || m[0] == 0xfa || m[0] == 0x76 || m[0] == 0x7a || m[0] == 0xfc) { *kind = 2; return 8; }
	*kind = 0;
	return 0;
}

static int walk_hits(const uint8_t * m, unsigned len, unsigned start, unsigned entry)
{
	unsigned pc = start;
	int hit = start == entry;

	while (pc + 2 <= len) {
		int kind;
		unsigned n = ilen(m + pc, len - pc, &kind);

		if (n == 0) return 0;
		if (m[pc] == 0x08 && m[pc + 1] == 0x01) {
			if (hit) return 1;
			if (pc + 4 > entry) return 0;
			pc += 4;
			while (pc + 2 <= len && m[pc] == 0x08 && m[pc + 1] == 0x00) pc += 2;
			if (pc == entry) hit = 1;
			continue;
		}
		pc += n;
		if (pc == entry) hit = 1;
	}
	return 0;
}

static unsigned long cnt[256][3];     /* occurrences by opcode and kind (known, assumed, unknown-stop) */
static unsigned long stops;
static unsigned long progs_unal, progs_walk, progs_stop, progs_zero;

#define MAXP 400
struct fprog { uint64_t entry, block; int dir; };
static struct fprog fp[MAXP];
static int nfp;
struct tex { uint32_t w[6]; uint64_t va; int form; uint32_t dest; int times; };
static struct tex texs[400];
static int ntex;

static void add_tex(uint32_t * w, uint32_t dest)
{
	int i;
	uint64_t va = 0;
	int form = s8s_texrec_va(w, &va);

	for (i = 0; i < ntex; i++) if (memcmp(texs[i].w, w, 24) == 0) { texs[i].times++; return; }
	if (ntex < 400) { memcpy(texs[ntex].w, w, 24); texs[ntex].va = va; texs[ntex].form = form; texs[ntex].dest = dest; texs[ntex].times = 1; ntex++; }
}

int main(int argc, char ** argv)
{
	int d, i, j, k;
	FILE * corp = 0;

	if (argc > 2 && strcmp(argv[1], "-o") == 0) {      /* -o corpus.bin: also write each program with a determined start as a corpus record ('f', pad[0] = 1 block / 2 walk, entry offset from start, bytes) */
		corp = fopen(argv[2], "wb");
		argv += 2;
		argc -= 2;
		argv[0] = (char *)"frag_forms";
	}
	for (d = 1; d < argc && d < 12; d++) {
		if (!load_dir(argv[d], &mems[d - 1])) { fprintf(stderr, "cannot load %s\n", argv[d]); return 2; }
		merge_in(&merged, &mems[d - 1]);
	}
	for (d = 1; d < argc && d < 12; d++) {
		static struct s8s_pass passes[32];
		static struct s8s_draw draws[512];
		long cllen = 0;
		uint8_t * cl;
		int np;

		cur = &mems[d - 1];
		cl = shm_of(cur, &cllen);
		if (!cl) continue;
		np = s8s_find_passes((const uint32_t *)cl, (uint32_t)(cllen / 4), passes, 32);
		for (i = 0; i < np; i++) {
			int nd = s8s_scan(rd, 0, passes[i].stream, (uint32_t)i, draws, 512, 0);
			struct s8s_prog pa;

			for (j = 0; j < nd; j++) {
				const struct s8s_draw * dr = &draws[j];

				if (dr->fs.found) {
					for (k = 0; k < nfp; k++) if (fp[k].entry == dr->fs.entry) break;
					if (k == nfp && nfp < MAXP) { fp[nfp].entry = dr->fs.entry; fp[nfp].block = dr->fs.block; fp[nfp].dir = d; nfp++; }
				}
				for (k = 0; k < (int)dr->fs.npush; k++) {
					if (dr->fs.push[k].type == S8S_PUSH_TEXTURE) {
						uint32_t w[6];
						int q, ok = 1;

						for (q = 0; q < 6; q++) if (!rd(0, dr->fs.push[k].va + 4 * q, &w[q])) ok = 0;
						if (ok) add_tex(w, dr->fs.push[k].dest);
					}
				}
			}
			/* the pass's own load/store programs (slots A and B) are fragment programs too */
			{
				uint32_t sl[2];
				int a;

				sl[0] = passes[i].slotA; sl[1] = passes[i].slotB;
				for (a = 0; a < 2; a++) {
					if (!sl[a]) continue;
					s8s_parse_slot(rd, 0, sl[a], &pa);
					if (pa.found) {
						for (k = 0; k < nfp; k++) if (fp[k].entry == pa.entry) break;
						if (k == nfp && nfp < MAXP) { fp[nfp].entry = pa.entry; fp[nfp].block = pa.block; fp[nfp].dir = d; nfp++; }
					}
					for (k = 0; k < (int)pa.npush; k++) {
						if (pa.push[k].type == S8S_PUSH_TEXTURE) {
							uint32_t w[6];
							int q, ok = 1;

							for (q = 0; q < 6; q++) if (!rd(0, pa.push[k].va + 4 * q, &w[q])) ok = 0;
							if (ok) add_tex(w, pa.push[k].dest);
						}
					}
				}
			}
		}
	}
	cur = &merged;
	printf("# %d distinct fragment program entries, %d distinct sampled texture records, memory merged from %d snapshots\n\n", nfp, ntex, argc - 1);
	printf("## FRAGMENT PROGRAMS (start = block+0x40 when a program block exists, else the entry; first 64 bytes split by instruction)\n\n");
	for (i = 0; i < nfp; i++) {
		struct reg * r = findreg(&merged, fp[i].entry);
		unsigned start, off, pc, end = 0;
		int ins = 0, stopped = 0, reliable = 0;
		unsigned long local[256];

		printf("### program %llx block %llx", (unsigned long long)fp[i].entry, (unsigned long long)fp[i].block);
		if (!r) { printf(" : region not in any capture\n\n"); progs_zero++; continue; }
		off = (unsigned)(fp[i].entry - r->va);
		start = off;
		reliable = 0;
		if (fp[i].block && findreg(&merged, fp[i].block) == r) { start = (unsigned)(fp[i].block - r->va) + 0x40; reliable = 1; }
		if (!(fp[i].block && findreg(&merged, fp[i].block) == r)) {
			unsigned q;

			for (q = off >= 0x100 ? off - 0x100 : 0; q <= off; q++) {
				if (walk_hits(r->m, (unsigned)r->len, q, off)) { start = q; reliable = 1; break; }
				if (walk_hits(r->m, (unsigned)r->len, q, off + 2)) { start = q; reliable = 1; printf("  (entry pointer flag bits 2: the instruction boundary is entry+2)\n"); break; }
			}
		}
		{
			unsigned z = 0, n;

			for (n = 0; n < 32 && start + n < r->len; n++) z |= r->m[start + n];
			if (!z) { printf(" : bytes are zero in all captures\n\n"); progs_zero++; continue; }
		}
		printf(" start %+d from entry\n", (int)start - (int)off);
		if (corp && reliable) {
			char kind = 'f';
			uint8_t pad[3] = { (uint8_t)((fp[i].block && findreg(&merged, fp[i].block) == r) ? 1 : 2), 0, 0 };
			uint32_t e = off - start, l = (uint32_t)(r->len - start > 2048 ? 2048 : r->len - start);

			fwrite(&kind, 1, 1, corp); fwrite(pad, 1, 3, corp); fwrite(&e, 4, 1, corp); fwrite(&l, 4, 1, corp); fwrite(r->m + start, 1, l, corp);
		}
		memset(local, 0, sizeof(local));
		pc = start;
		while (pc + 2 <= r->len) {
			int kind;
			unsigned n = ilen(r->m + pc, (unsigned)(r->len - pc), &kind);
			unsigned b;

			if (n == 0) {
				if (pc - start < 64) printf("  %04x: %02x ?? unknown length (bytes %02x %02x %02x %02x %02x %02x)\n", pc - start, r->m[pc], r->m[pc], r->m[pc + 1], r->m[pc + 2], r->m[pc + 3], r->m[pc + 4], r->m[pc + 5]);
				if (reliable) { cnt[r->m[pc]][2]++; stops++; stopped = 1; }
				else { stopped = 2; }
				break;
			}
			if (pc - start < 64) {
				printf("  %04x%s %-15s", pc - start, pc == off ? "*" : " ", opname(r->m[pc]));
				for (b = 0; b < n; b++) printf(" %02x", r->m[pc + b]);
				printf("%s\n", kind == 2 ? "   (length assumed)" : "");
			}
			cnt[r->m[pc]][kind - 1]++;
			local[r->m[pc]]++;
			ins++;
			if (r->m[pc] == 0x08 && r->m[pc + 1] == 0x01) {
				if (pc >= off) { end = pc + 4; break; }
				pc += 4;
				while (pc + 2 <= r->len && r->m[pc] == 0x08 && r->m[pc + 1] == 0x00) pc += 2;
				continue;
			}
			pc += n;
		}
		if (stopped == 2) { progs_unal++; printf("  -> start of the program not determinable (no program block, no walk from an earlier address reaches the entry): not counted\n"); }
		else if (stopped) { progs_stop++; printf("  -> walk stopped at +0x%x after %d instructions (entry is at +0x%x)\n", pc - start, ins, off - start); }
		else { progs_walk++; printf("  -> reaches end instruction at +0x%x, %d instructions, entry at +0x%x\n", end - start, ins, off - start); }
		printf("\n");
	}
	printf("## OPCODE OCCURRENCES over all programs above (instructions walked before the walk stopped)\n\n");
	printf("programs: %lu walk to end, %lu stop at an unknown form, %lu start not determinable, %lu without bytes\n\n", progs_walk, progs_stop, progs_unal, progs_zero);
	printf("opcode  guess            known-len  assumed-len  UNKNOWN-stop\n");
	for (i = 0; i < 256; i++) {
		if (cnt[i][0] || cnt[i][1] || cnt[i][2]) printf("  0x%02x  %-15s %8lu %11lu %12lu\n", i, opname((uint8_t)i), cnt[i][0], cnt[i][1], cnt[i][2]);
	}
	printf("\n## SAMPLED TEXTURES (type-0x17 pushes: texture-state record words)\n\n");
	for (i = 0; i < ntex; i++) {
		printf("TEX %08x %08x %08x %08x %08x %08x  va %llx form %d dest %u uses %d\n", texs[i].w[0], texs[i].w[1], texs[i].w[2], texs[i].w[3], texs[i].w[4], texs[i].w[5],
		    (unsigned long long)texs[i].va, texs[i].form, texs[i].dest, texs[i].times);
	}
	return 0;
}
