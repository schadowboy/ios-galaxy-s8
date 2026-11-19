/*
 * stream_decode <snapshot dir> [-v]: decode the compositor command stream of a capture17 session snapshot (tests/replay_dump.py) with core/s8gfx_stream.h,
 * print the draw list and check it: draw/program pointers must be mapped, vertex and fragment programs must walk to their `08 01` end (s8f_len),
 * indexed draws must have plausible indices, every pass must find its render target. Exit code 1 if a hard check fails.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s8gfx_frag.h"
#include "s8gfx_stream.h"

struct reg { char name[32]; uint64_t va, len; uint8_t * m; };
static struct reg regs[256];
static int nregs;
static int verbose;

static struct reg * findreg(uint64_t va)
{
	int i;

	for (i = 0; i < nregs; i++) {
		if (regs[i].va != 0 && va >= regs[i].va && va < regs[i].va + regs[i].len) {
			return &regs[i];
		}
	}
	return 0;
}

static int rd(void * ctx, uint64_t va, uint32_t * out)
{
	struct reg * r = findreg(va);

	(void)ctx;
	if (r == 0 || va + 4 > r->va + r->len) {
		return 0;
	}
	memcpy(out, r->m + (va - r->va), 4);
	return 1;
}

static uint8_t * ptr(uint64_t va, unsigned * avail)
{
	struct reg * r = findreg(va);

	if (r == 0) return 0;
	*avail = (unsigned)(r->va + r->len - va);
	return r->m + (va - r->va);
}

static unsigned failpc, failop;
static int g_hit;
static int blk_total, blk_entry, blk_full;

/* instruction length: core's s8f_len plus the one extra form this decode found (0xf9 = ld_tile with bit 7, 8 B, in the compositor fragment prologue) */
static unsigned ilen(const uint8_t * m, unsigned avail)
{
	/* forms seen only in the compositor's fragment programs and not in s8f_len (probe: lengths assumed from the neighbouring 8-byte forms; the walk from the
	 * program block start landing exactly on the entry pointer is the evidence, see analysis/compositor-stream.md section 7) */
	if (m[0] == 0xf9 || m[0] == 0xf6 || m[0] == 0xfa || m[0] == 0xfc || m[0] == 0x76 || m[0] == 0x7a) return 8;
	return s8f_len(m, avail);
}

/* walk instructions from m[q] to an `08 01`; returns 1 and the end offset */
static int walk(const uint8_t * m, unsigned len, unsigned q, unsigned * endp)
{
	unsigned pc = q;

	while (pc + 2 <= len) {
		unsigned n = ilen(m + pc, len - pc);

		if (n == 0) { failpc = pc; failop = m[pc]; return 0; }
		if (m[pc] == 0x08 && m[pc + 1] == 0x01) { *endp = pc + 4; return 1; }
		pc += n;
	}
	return 0;
}

/* walk from `start` and report whether `entry` is an instruction boundary on the way */
static int walk_hits(const uint8_t * m, unsigned len, unsigned start, unsigned entry, unsigned * endp)
{
	unsigned pc = start;
	int hit = start == entry;

	failpc = 0xffffffffu;
	failop = 0;
	while (pc + 2 <= len) {
		unsigned n = ilen(m + pc, len - pc);

		if (n == 0) { failpc = pc; failop = m[pc]; g_hit = hit; return 0; }
		if (m[pc] == 0x08 && m[pc + 1] == 0x01) {
			*endp = pc + 4;
			if (hit || pc + 4 > entry) { g_hit = hit; return hit; }
			/* a program block can hold several shaders: skip the `08 00` padding after this end and go on to the next one */
			pc += 4;
			while (pc + 2 <= len && m[pc] == 0x08 && m[pc + 1] == 0x00) pc += 2;
			if (pc == entry) hit = 1;
			continue;
		}
		pc += n;
		if (pc == entry) hit = 1;
	}
	g_hit = hit;
	return 0;
}

static int iszero(const uint8_t * p, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++) if (p[i]) return 0;
	return 1;
}

static int check_vertex(const struct s8s_draw * d, char * msg)
{
	unsigned avail, end = 0, off, st;
	struct reg * r = findreg(d->vs.entry);
	uint8_t * p = ptr(d->vs.entry, &avail);

	if (!r || !p) { strcpy(msg, "region not in the capture"); return 3; }
	off = (unsigned)(d->vs.entry - r->va);
	if (iszero(r->m + off, 64 < avail ? 64 : avail)) { strcpy(msg, "program bytes not in the capture (zero)"); return 3; }
	st = s8v_find_start(r->m, (unsigned)r->len, off);
	if (walk(r->m, (unsigned)r->len, st, &end)) {
		sprintf(msg, "start %+d end %+d", (int)st - (int)off, (int)end - (int)off);
		return 1;
	}
	sprintf(msg, "walk fails from start %+d at %+d op %02x", (int)st - (int)off, (int)failpc - (int)off, failop);
	return 0;
}

static int check_fragment(const struct s8s_draw * d, char * msg)
{
	struct reg * r = findreg(d->fs.entry);
	unsigned off, end = 0;

	if (!r) { strcpy(msg, "region not in the capture"); return 3; }
	off = (unsigned)(d->fs.entry - r->va);
	if (iszero(r->m + off, 32 < r->len - off ? 32 : (unsigned)(r->len - off))) { strcpy(msg, "program bytes not in the capture (zero)"); return 3; }
	if (d->fs.block && findreg(d->fs.block) == r) {
		unsigned b = (unsigned)(d->fs.block - r->va);

		if (iszero(r->m + b + 0x40, 0x10) || iszero(r->m + off - 16, 16)) { strcpy(msg, "program bytes not in the capture (zero)"); return 3; }
		blk_total++;
		if (walk_hits(r->m, (unsigned)r->len, b + 0x40, off, &end)) {
			blk_entry++;
			blk_full++;
			sprintf(msg, "block+0x40 start, entry at +0x%x, end +0x%x", off - b - 0x40, end - b - 0x40);
			return 1;
		}
		if (g_hit) blk_entry++;
		if (failpc == 0xffffffffu) sprintf(msg, "block+0x40 walk %s entry (+0x%x), ends at 08 01 before it; ", g_hit ? "reaches" : "MISSES", off - b - 0x40);
		else sprintf(msg, "block+0x40 walk %s entry (+0x%x) then stops at +0x%x op %02x; ", g_hit ? "reaches" : "MISSES", off - b - 0x40, failpc - b - 0x40, failop);
		msg += strlen(msg);
		if (g_hit) { strcpy(msg, "entry confirmed"); return 2; }
	}
	{   /* earliest start within 0x100 before the entry whose walk hits the entry and ends */
		unsigned q;

		for (q = off >= 0x100 ? off - 0x100 : 0; q <= off; q++) {
			if (walk_hits(r->m, (unsigned)r->len, q, off, &end)) {
				sprintf(msg, "search start %+d end %+d%s", (int)q - (int)off, (int)end - (int)off, d->fs.block ? " (block+0x40 did NOT work)" : "");
				return 2;
			}
		}
	}
	strcpy(msg, "no start found");
	return 0;
}

static float f32(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

int main(int argc, char ** argv)
{
	char path[512], line[256];
	FILE * f;
	static struct s8s_pass passes[32];
	static struct s8s_draw draws[S8S_MAXDRAW];
	int vbtotal = 0, vbgood = 0, skipped = 0;
	int smp[4][8], a, b;
	int np, p, hard = 0, total = 0, vzero = 0, fzero = 0, vok = 0, vbad = 0, fok = 0, fbad = 0, fsearch = 0, blendn = 0, indexed = 0, idxbad = 0, notgt = 0;
	uint8_t * cl = 0;
	memset(smp, 0, sizeof(smp));
	long cllen = 0;
	int i;

	if (argc < 2) { fprintf(stderr, "usage: stream_decode <snapshot dir> [-v]\n"); return 2; }
	for (i = 2; i < argc; i++) if (strcmp(argv[i], "-v") == 0) verbose = 1;
	snprintf(path, sizeof(path), "%s/regions.txt", argv[1]);
	f = fopen(path, "r");
	if (!f) { fprintf(stderr, "no %s\n", path); return 2; }
	while (fgets(line, sizeof(line), f) && nregs < 256) {
		struct reg * r = &regs[nregs];
		unsigned long long va, len;
		FILE * g;

		if (sscanf(line, "%31s %llx %llx", r->name, &va, &len) != 3) continue;
		r->va = va; r->len = len;
		snprintf(path, sizeof(path), "%s/%s.bin", argv[1], r->name);
		g = fopen(path, "rb");
		if (!g) continue;
		r->m = (uint8_t *)calloc(1, len);
		if (fread(r->m, 1, len, g) < 1) {}
		fclose(g);
		if (strcmp(r->name, "shmem_100") == 0) { cl = r->m; cllen = (long)len; r->va = 0; }
		nregs++;
	}
	fclose(f);
	/* extra snapshot dirs (any argument that is not -v): merge ONLY the program heaps (res_3, res_10, res_59, res_68..77 by VA) into the primary memory where a byte is zero */
	for (i = 2; i < argc; i++) {
		int n0 = nregs, q;

		if (argv[i][0] == '-') continue;
		snprintf(path, sizeof(path), "%s/regions.txt", argv[i]);
		f = fopen(path, "r");
		if (!f) continue;
		while (fgets(line, sizeof(line), f)) {
			char nm[32];
			unsigned long long va, len;
			FILE * g;
			uint8_t * buf;

			if (sscanf(line, "%31s %llx %llx", nm, &va, &len) != 3) continue;
			if (!(va == 0x100020000ULL || va == 0x1000e0000ULL || va == 0x100610000ULL || (va >= 0x1006b0000ULL && va < 0x100890000ULL))) continue;
			snprintf(path, sizeof(path), "%s/%s.bin", argv[i], nm);
			g = fopen(path, "rb");
			if (!g) continue;
			buf = (uint8_t *)calloc(1, len);
			if (fread(buf, 1, len, g) < 1) {}
			fclose(g);
			for (q = 0; q < n0; q++) {
				if (regs[q].va == va && regs[q].len == len) {
					uint64_t b;

					for (b = 0; b < len; b++) if (regs[q].m[b] == 0) regs[q].m[b] = buf[b];
					break;
				}
			}
			if (q == n0 && nregs < 256) {
				snprintf(regs[nregs].name, sizeof(regs[nregs].name), "%s", nm);
				regs[nregs].va = va; regs[nregs].len = len; regs[nregs].m = buf;
				nregs++;
				buf = 0;
			}
			if (buf) free(buf);
		}
		fclose(f);
	}
	if (!cl) { fprintf(stderr, "no shmem_100\n"); return 2; }

	np = s8s_find_passes((const uint32_t *)cl, (uint32_t)(cllen / 4), passes, 32);
	printf("%d passes in the kernel command list\n", np);
	for (p = 0; p < np; p++) {
		struct s8s_stats st;
		int nd;

		s8s_pass_target(rd, 0, &passes[p]);
		nd = s8s_scan(rd, 0, passes[p].stream, (uint32_t)p, draws, S8S_MAXDRAW, &st);
		printf("pass %d: block 0x%x %ux%u stream %llx  target %s %llx fmt %08x w1 %08x (form %u)  -> %d draws, %u links, %u loads, %u unknown words, %u unknown loads, end %d\n",
		    p, passes[p].block, passes[p].width, passes[p].height, (unsigned long long)passes[p].stream, passes[p].has_target ? "at" : "NONE",
		    (unsigned long long)passes[p].target_va, passes[p].target_fmt, passes[p].target_w1, passes[p].target_form, nd, st.links, st.loads,
		    st.unknown_words, st.unknown_loads, st.end_seen);
		if (!passes[p].has_target) notgt++;
		{   /* the chunk right behind the head LINK (skipped by the link): its first draw */
			static struct s8s_draw pre[2];
			struct s8s_stats st2;
			int n2 = s8s_scan(rd, 0, passes[p].stream + 8, (uint32_t)p, pre, 2, &st2);

			if (n2 > 0 && pre[0].kind == S8S_DRAW_OTHER) {
				/* the head LINK overwrote the chunk's A record, so the chunk starts with B; its fragment program is the pass's slot A (the clear / load program) */
				struct s8s_prog vsb, fa;
				uint32_t bptr = 0;
				int ok = rd(0, passes[p].stream + 8, &bptr);

				s8s_parse_slot(rd, 0, ok ? bptr : 0, &vsb);
				s8s_parse_slot(rd, 0, passes[p].slotA, &fa);
				printf("  skipped chunk @%04x: 7e44 draw ptr %llx, vs %llx (B slot %llx) fs = pass slot A %llx hdr %08x entry %llx; slot A pushes:", (unsigned)(pre[0].pc & 0xffff),
				    (unsigned long long)pre[0].index_va, (unsigned long long)vsb.entry, (unsigned long long)vsb.slot, (unsigned long long)fa.slot, fa.hdr,
				    (unsigned long long)fa.entry);
				for (i = 0; i < (int)fa.npush; i++) {
					uint32_t q[4] = { 0, 0, 0, 0 };
					int k;

					for (k = 0; k < 4; k++) rd(0, fa.push[i].va + 4 * k, &q[k]);
					printf(" [%02x dest %02x %llx: %g %g %g %g]", fa.push[i].type, fa.push[i].dest, (unsigned long long)fa.push[i].va, f32(q[0]), f32(q[1]), f32(q[2]), f32(q[3]));
				}
				printf("\n");
				skipped++;
			}
		}
		for (i = 0; i < nd; i++) {
			const struct s8s_draw * d = &draws[i];
			char vm[96] = "", fm[96] = "";
			int j, vv, ff;

			total++;
			vv = d->vs.found ? check_vertex(d, vm) : 0;
			ff = d->fs.found ? check_fragment(d, fm) : 0;
			if (d->kind != S8S_DRAW_OTHER) {
				if (vv == 3) vzero++; else if (vv) vok++; else vbad++;
				if (ff == 3) fzero++; else if (ff == 1) fok++; else if (ff == 2) { fok++; fsearch++; } else fbad++;
			}
			blendn += d->blend;
			if (d->kind != S8S_DRAW_OTHER) {
				int nt = 0;
				uint64_t vbva = 0, texva = 0;
				uint32_t lo = 0, hi = 0;

				for (j = 0; j < (int)d->fs.npush; j++) {
					if (d->fs.push[j].type == S8S_PUSH_TEXTURE) {
						uint32_t w[6];
						int k;

						for (k = 0; k < 6; k++) { w[k] = 0; rd(0, d->fs.push[j].va + 4 * k, &w[k]); }
						if (nt == 0 && s8s_texrec_va(w, &texva) == 0) texva = 0;
						nt++;
					}
				}
				for (j = 0; j < (int)d->vs.npush; j++) {
					if (d->vs.push[j].halves == 4 && d->vs.push[j].type == S8S_PUSH_UNIFORM) {
						rd(0, d->vs.push[j].va, &lo);
						rd(0, d->vs.push[j].va + 4, &hi);
						vbva = ((uint64_t)hi << 32) | lo;
						break;
					}
				}
				if (nt < 4) smp[nt][d->sampler[1] < 8 ? d->sampler[1] : 7]++;
				printf("  DL p%d.%d %s prim %u n %u vs %llx[%s] fs %llx[%s] vb %llx %s%llx tex %d%s blend %u target %llx\n", p, i, d->kind == S8S_DRAW_INDEXED ? "idx" : "lst", d->prim, d->count,
				    (unsigned long long)d->vs.entry, vv == 1 ? "walk-ok" : vv == 3 ? "no-bytes" : "FAIL", (unsigned long long)d->fs.entry, ff == 1 ? "walk-ok" : ff == 2 ? "entry-ok" : ff == 3 ? "no-bytes" : "unwalked", (unsigned long long)vbva, d->kind == S8S_DRAW_INDEXED ? "ib " : "",
				    (unsigned long long)(d->kind == S8S_DRAW_INDEXED ? d->index_va : 0), nt, nt ? "" : "", d->blend, (unsigned long long)passes[p].target_va);
				if (texva) printf("     texture0 at %llx\n", (unsigned long long)texva);
			}
			printf("  draw %2d @%04x kind %d prim %u count %u x%u", i, (unsigned)(d->pc & 0xffff), d->kind, d->prim, d->count, d->instances);
			if (d->kind != S8S_DRAW_LIST) printf(" idx %llx", (unsigned long long)d->index_va);
			printf("\n    vs %llx[%x] regs %u slot %llx k%u hdr %08x | fs %llx[%x] extra %llx block %llx tail %llx class %u slot %llx hdr %08x | blend %u\n",
			    (unsigned long long)d->vs.entry, d->vs.entry_flags, d->vs_regs, (unsigned long long)d->vs.slot, d->vs.slot_k, d->vs.hdr,
			    (unsigned long long)d->fs.entry, d->fs.entry_flags, (unsigned long long)d->fs.extra, (unsigned long long)d->fs.block,
			    (unsigned long long)d->fs_tail, d->fs_regs, (unsigned long long)d->fs.slot, d->fs.hdr, d->blend);
			printf("    vs prog: %s   fs prog: %s\n", vm, fm);
			if (d->kind == S8S_DRAW_INDEXED) {
				unsigned k, mx = 0, bad = 0;
				uint32_t w;

				indexed++;
				for (k = 0; k < d->count; k += 2) {
					if (!rd(0, d->index_va + k * 2, &w)) { bad++; break; }
					if ((w & 0xffff) > mx) mx = w & 0xffff;
					if (k + 1 < d->count && (w >> 16) > mx) mx = w >> 16;
				}
				printf("    indices u16: max %u%s\n", mx, bad ? " UNMAPPED" : "");
				if (bad || mx > 4096) idxbad++;
			}
			{   /* vertex buffer: the vertex-stage push with destination 0 and 4 halves holds a 64-bit pointer {lo, 1}; stride 0x30 (index data follows 4 vertices) */
				int j;

				for (j = 0; j < (int)d->vs.npush; j++) {
					if (d->vs.push[j].halves == 4 && d->vs.push[j].type == S8S_PUSH_UNIFORM) {
						uint32_t lo = 0, hi = 0, w[4];
						uint64_t vb;
						int k, v, good = 1;

						rd(0, d->vs.push[j].va, &lo);
						rd(0, d->vs.push[j].va + 4, &hi);
						vb = ((uint64_t)hi << 32) | lo;
						printf("    vertex buffer %llx:", (unsigned long long)vb);
						for (v = 0; v < 4 && v < (int)d->count; v++) {
							for (k = 0; k < 4; k++) {
								w[k] = 0;
								if (!rd(0, vb + 0x30 * v + 4 * k, &w[k])) good = 0;
							}
							printf("  (%g,%g,%g,%g)", f32(w[0]), f32(w[1]), f32(w[2]), f32(w[3]));
							if (!(f32(w[0]) > -8000.0f && f32(w[0]) < 8000.0f && f32(w[1]) > -8000.0f && f32(w[1]) < 8000.0f)) good = 0;
						}
						printf("%s\n", good ? "" : "  [not plausible]");
						vbtotal++;
						if (good) vbgood++;
						break;
					}
				}
			}
			if (verbose || i == 0) {
				printf("    viewport %s tx %g sx %g ty %g sy %g tiles x %u..%u y %u..%u | sampler %08x %08x %08x | pipe %08x %08x\n", d->vp_valid ? "ok" : "none",
				    f32(d->vp[0]), f32(d->vp[1]), f32(d->vp[2]), f32(d->vp[3]), d->tile_x0, d->tile_x1, d->tile_y0, d->tile_y1, d->sampler[0],
				    d->sampler[1], d->sampler[2], d->pipe[1], d->pipe[2]);
			}
			if (verbose) {
				for (j = 0; j < (int)d->vs.npush; j++) {
					uint32_t a = 0, b = 0;

					rd(0, d->vs.push[j].va, &a);
					rd(0, d->vs.push[j].va + 4, &b);
					printf("    vs push %02x dest %02x va %llx halves %u : %08x %08x\n", d->vs.push[j].type, d->vs.push[j].dest,
					    (unsigned long long)d->vs.push[j].va, d->vs.push[j].halves, a, b);
				}
				for (j = 0; j < (int)d->fs.npush; j++) {
					uint32_t w[6];
					uint64_t va;
					int k, form;

					for (k = 0; k < 6; k++) { w[k] = 0; rd(0, d->fs.push[j].va + 4 * k, &w[k]); }
					form = d->fs.push[j].type == S8S_PUSH_TEXTURE ? s8s_texrec_va(w, &va) : 0;
					printf("    fs push %02x dest %02x va %llx halves %u : %08x %08x %08x %08x", d->fs.push[j].type, d->fs.push[j].dest,
					    (unsigned long long)d->fs.push[j].va, d->fs.push[j].halves, w[0], w[1], w[2], w[3]);
					if (form) printf("  -> texture at %llx (form %d)", (unsigned long long)va, form);
					printf("\n");
				}
			}
			if (d->kind != S8S_DRAW_OTHER && (!d->vs.found || !d->fs.found || (d->kind == S8S_DRAW_LIST && d->count == 0))) {
				if (st.links == 0 || !st.end_seen) {
					/* a pass block that no longer belongs to this session's stream (stale shmem block in a delta replay) */
					printf("    WARN: pass %d is stale in this replay (stream does not start with a LINK)\n", p);
				} else {
					printf("    HARD: missing %s%s\n", d->vs.found ? "" : "vertex slot ", d->fs.found ? "" : "fragment slot");
					hard++;
				}
			}
		}
	}
	printf("TOTAL %d draws: vertex programs walk %d ok / %d bad / %d not captured, fragment programs walk %d ok (%d by search, not via block+0x40) / %d bad / %d not captured, %d with blend, %d indexed (%d implausible), %d passes without target\n",
	    total, vok, vbad, vzero, fok, fsearch, fbad, fzero, blendn, indexed, idxbad, notgt);
	printf("texture pushes per draw vs sampler-load word 1 (rows: 0..3 textures, columns: value 0..6, 7 = more):\n");
	for (a = 0; a < 4; a++) {
		printf("  %d tex:", a);
		for (b = 0; b < 8; b++) printf(" %3d", smp[a][b]);
		printf("\n");
	}
	printf("passes with a skipped first chunk holding a 7e44 draw: %d of %d\n", skipped, np);
	printf("vertex buffers found via the destination-0 push: %d, first 4 positions plausible (|x|,|y| < 8000, mapped): %d\n", vbtotal, vbgood);
	printf("fragment programs with a program block record: %d, walk from block+0x40 lands on the entry pointer: %d, ... and then reaches 08 01: %d\n", blk_total, blk_entry, blk_full);
	printf("%s\n", hard == 0 ? "stream_decode: PASS" : "stream_decode: FAIL");
	return hard != 0;
}
