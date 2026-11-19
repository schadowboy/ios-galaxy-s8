/*
 * comp_replay <snapshot dir> [extra dirs for program heaps] [-v] [-s N]: host reproduction of AGXDeviceUserClient::execComposite() (core/S8Agx.cpp.reference)
 * on a capture snapshot (tests/replay_dump.py). For every live pass and every draw of the s8gfx_stream.h draw list it does what the kernel does:
 *   vertex stage: bind d->vs pushes into the uniform file, s8v_find_start, s8f_run in vertex mode per vertex id (indices for indexed draws), viewport transform
 *                 with d->vp, report ok / failed vertices (unknown op, pc_fail, expmask), the positions in pixels and whether they land inside width x height;
 *   fragment stage: s8f_start_image (block image or walk-back), uniforms from d->fs pushes, a CONSTANT synthetic texture for every 0x17 push (the texels are not in
 *                 the dump), rasterise with s8r_tri and run the fragment program for sampled covered pixels (every -s N th, default 211): stored / unknown / discard.
 * Prints one block per draw and a summary. Exit code 0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int g_trace;
struct s8f_ctx;
static void trace_fn(const struct s8f_ctx * c, unsigned pc, const unsigned char * b);
#define S8F_TRACE(c, pc, b) trace_fn(c, pc, b)
#include "s8gfx_frag.h"
#include "s8gfx_rast.h"
#include "s8gfx_stream.h"

struct reg { char name[32]; uint64_t va, len; uint8_t * m; };
static struct reg regs[300];
static int nregs;

static struct reg * findreg(uint64_t va)
{
	int i;

	for (i = 0; i < nregs; i++) if (regs[i].va && va >= regs[i].va && va < regs[i].va + regs[i].len) return &regs[i];
	return 0;
}

static int streamrd(void * ctx, uint64_t va, uint32_t * out)
{
	struct reg * r = findreg(va);

	(void)ctx;
	if (!r || va + 4 > r->va + r->len) { *out = 0; return 0; }
	memcpy(out, r->m + (va - r->va), 4);
	return 1;
}

static uint32_t agxrd(void * ctx, uint64_t va)
{
	uint32_t v;

	streamrd(ctx, va, &v);
	return v;
}

static int resregion(uint64_t va, const uint8_t ** base, uint64_t * off, uint64_t * len)
{
	struct reg * r = findreg(va);

	if (!r) return 0;
	*base = r->m; *off = va - r->va; *len = r->len;
	return 1;
}

static float f32(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

static void trace_fn(const struct s8f_ctx * c, unsigned pc, const unsigned char * b)
{
	int i;

	if (!g_trace) return;
	printf("        %03x: %02x %02x %02x %02x %02x %02x %02x %02x | r0..r11:", pc, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
	for (i = 0; i < 12; i++) printf(" %08x", c->r[i]);
	printf("\n");
}

static void bind_uniforms(const struct s8s_prog * pg, uint32_t * uni)
{
	uint32_t k, j;

	for (k = 0; k < pg->npush; k++) {
		const struct s8s_push * pu = &pg->push[k];
		uint32_t halves;

		if (pu->type != 0x03 && pu->type != 0x33) continue;
		halves = pu->halves ? pu->halves : 32;
		for (j = 0; j < halves / 2 && pu->dest + 2 * j < 128; j++) uni[pu->dest + 2 * j] = agxrd(0, pu->va + 4 * j);
	}
}

static int trace_pass = -1, trace_draw = -1, ftrace_pass = -1, ftrace_draw = -1;
static uint32_t const_tex(void * ctx, uint64_t va) { (void)ctx; (void)va; return 0xff9a6a3au; }

struct shade { int trace; struct s8f_ctx tmpl; const uint8_t * frag; uint32_t fraglen; int w, h, step; uint32_t cnt, shaded, stored, unknown, discard, noend, first_fail_pc; uint32_t ex, ey, out[4]; };

static void emit(void * vctx, int px, int py, const f32b * v)
{
	struct shade * s = (struct shade *)vctx;
	struct s8f_ctx fc;

	if (px < 0 || py < 0 || px >= s->w || py >= s->h) return;
	s->cnt++;
	if ((s->cnt - 1) % (uint32_t)s->step != 0) return;
	memcpy(&fc, &s->tmpl, sizeof(fc));
	fc.varying = v;
	fc.tile = 0xff202020u;
	fc.has_tile = 1;
	fc.sr[48] = (uint32_t)px & 31;
	fc.sr[49] = (uint32_t)py & 31;
	g_trace = s->trace && s->shaded == 0;
	if (g_trace) printf("      TRACE fragment pixel (%d,%d) varyings %08x %08x %08x %08x\n", px, py, v[0], v[1], v[2], v[3]);
	s->shaded++;
	if (s8f_run(&fc, s->frag, s->fraglen) && fc.stored) {
		g_trace = 0;
		if (s->stored == 0) { s->ex = (uint32_t)px; s->ey = (uint32_t)py; memcpy(s->out, fc.out, 16); }
		s->stored++;
	} else if (fc.unknown) { s->unknown = (uint32_t)fc.unknown; s->first_fail_pc = fc.pc_fail; }
	else if (fc.discard) s->discard++;
	else s->noend++;
}

int main(int argc, char ** argv)
{
	char path[512], line[256];
	FILE * f;
	static struct s8s_pass passes[32];
	static struct s8s_draw draws[512];
	static struct s8r_vtx vt[1024];
	static uint8_t vok[1024];
	uint8_t * cl = 0;
	long cllen = 0;
	int i, np, p, verbose = 0, step = 211;
	int nd_total = 0, nd_run = 0, nv_ok_draws = 0, nv_fail_draws = 0, onscreen_draws = 0, frag_store_draws = 0, frag_nostore_draws = 0, skipped = 0;
	static int unkv[256], unkf[256];
	int vzero_draws = 0;

	if (argc < 2) { fprintf(stderr, "usage: comp_replay <dir> [extra dirs] [-v] [-s N]\n"); return 2; }
	for (i = 2; i < argc; i++) { if (!strcmp(argv[i], "-v")) verbose = 1; if (!strcmp(argv[i], "-s") && i + 1 < argc) step = atoi(argv[++i]); if (!strcmp(argv[i], "-tf") && i + 2 < argc) { ftrace_pass = atoi(argv[i + 1]); ftrace_draw = atoi(argv[i + 2]); i += 2; } else if (!strcmp(argv[i], "-t") && i + 2 < argc) { trace_pass = atoi(argv[i + 1]); trace_draw = atoi(argv[i + 2]); i += 2; } }
	snprintf(path, sizeof(path), "%s/regions.txt", argv[1]);
	f = fopen(path, "r");
	if (!f) { fprintf(stderr, "no %s\n", path); return 2; }
	while (fgets(line, sizeof(line), f) && nregs < 280) {
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
		if (!strcmp(r->name, "shmem_100")) { cl = r->m; cllen = (long)len; r->va = 0; }
		nregs++;
	}
	fclose(f);
	for (i = 2; i < argc; i++) {     /* program heaps from other snapshots fill zero bytes */
		if (argv[i][0] == '-' || (i > 2 && (!strcmp(argv[i - 1], "-s") || !strcmp(argv[i - 1], "-t") || !strcmp(argv[i - 2], "-t") || !strcmp(argv[i - 1], "-tf") || !strcmp(argv[i - 2], "-tf")))) continue;
		snprintf(path, sizeof(path), "%s/regions.txt", argv[i]);
		f = fopen(path, "r");
		if (!f) continue;
		while (fgets(line, sizeof(line), f)) {
			char nm[32];
			unsigned long long va, len;
			FILE * g;
			uint8_t * buf;
			int q, found = 0;

			if (sscanf(line, "%31s %llx %llx", nm, &va, &len) != 3) continue;
			if (!(va == 0x100020000ULL || va == 0x1000e0000ULL || va == 0x100610000ULL || (va >= 0x1006b0000ULL && va < 0x100890000ULL))) continue;
			snprintf(path, sizeof(path), "%s/%s.bin", argv[i], nm);
			g = fopen(path, "rb");
			if (!g) continue;
			buf = (uint8_t *)calloc(1, len);
			if (fread(buf, 1, len, g) < 1) {}
			fclose(g);
			for (q = 0; q < nregs; q++) if (regs[q].va == va && regs[q].len == len) {
				uint64_t b;

				for (b = 0; b < len; b++) if (!regs[q].m[b]) regs[q].m[b] = buf[b];
				found = 1;
				break;
			}
			if (!found && nregs < 299) { snprintf(regs[nregs].name, 32, "%s", nm); regs[nregs].va = va; regs[nregs].len = len; regs[nregs].m = buf; nregs++; }
			else free(buf);
		}
		fclose(f);
	}
	if (!cl) { fprintf(stderr, "no shmem_100\n"); return 2; }

	np = s8s_find_passes((const uint32_t *)cl, (uint32_t)(cllen / 4), passes, 32);
	for (p = 0; p < np; p++) {
		struct s8s_pass * ps = &passes[p];
		uint32_t first = 0;
		struct s8s_stats st;
		int nd, di;

		if (!streamrd(0, ps->stream, &first) || first != 0x80000001u || ps->width == 0 || ps->height == 0 || ps->width > 4096 || ps->height > 4096) { skipped++; continue; }
		s8s_pass_target(streamrd, 0, ps);
		nd = s8s_scan(streamrd, 0, ps->stream, (uint32_t)p, draws, 512, &st);
		printf("== pass %d %ux%u target %llx %s, %d draws\n", p, ps->width, ps->height, (unsigned long long)ps->target_va, ps->has_target ? "" : "NONE", nd);
		for (di = 0; di < nd; di++) {
			struct s8s_draw * d = &draws[di];
			const uint8_t * vbase, * fbase;
			uint64_t voff, vlen, foff, flen;
			unsigned vstart, fstart = 0;
			uint32_t vp[4], uni[128], tried = 0, ok = 0, fail = 0, k, t;
			uint32_t fu = 0, fpc = 0, fmask = 0, idxv[3];
			int inside = 0, outside = 0, nvar = 0, simg = 0;
			float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
			struct shade sh;
			uint32_t ntri;

			nd_total++;
			if (d->kind == S8S_DRAW_OTHER || d->count < 3 || d->count > 4000 || (d->prim != 6 && d->prim != 9)) { printf("  draw %d: skipped (kind %u prim %u count %u)\n", di, d->kind, d->prim, d->count); continue; }
			if (!resregion(d->vs.entry, &vbase, &voff, &vlen) || !resregion(d->fs.entry, &fbase, &foff, &flen)) { printf("  draw %d: program region not in the capture (vs %llx fs %llx)\n", di, (unsigned long long)d->vs.entry, (unsigned long long)d->fs.entry); continue; }
			nd_run++;
			vstart = s8v_find_start(vbase, (unsigned)(vlen > 0xffffffffULL ? 0xffffffffU : vlen), (unsigned)voff);
			if (d->fs.block != 0 && resregion(d->fs.block, &fbase, &foff, &flen)) {
				unsigned blk = (unsigned)foff, ent = 0, fs0 = 0;
				uint64_t eoff, el;
				const uint8_t * eb;

				if (resregion(d->fs.entry, &eb, &eoff, &el) && eb == fbase) ent = (unsigned)eoff;
				simg = s8f_start_image(fbase, (unsigned)flen, blk, ent, &fs0);
				fstart = simg ? fs0 : blk + 0x40;
			} else if (resregion(d->fs.entry, &fbase, &foff, &flen)) {
				unsigned fs0 = 0;

				simg = s8f_start_image(fbase, (unsigned)flen, (unsigned)-1, (unsigned)foff, &fs0);
				fstart = simg ? fs0 : (unsigned)foff;
			}
			if (d->vp_valid) memcpy(vp, d->vp, sizeof(vp));
			else { vp[0] = vp[1] = sf_from_i64((int64_t)ps->width / 2); vp[2] = sf_from_i64((int64_t)ps->height / 2); vp[3] = sf_from_i64(-(int64_t)ps->height / 2); }
			memset(uni, 0, sizeof(uni));
			bind_uniforms(&d->vs, uni);
			memset(vok, 0, sizeof(vok));
			ntri = d->count;
			memset(&sh, 0, sizeof(sh));
			sh.trace = (p == ftrace_pass && di == ftrace_draw);
			sh.w = (int)ps->width; sh.h = (int)ps->height; sh.step = step;
			bind_uniforms(&d->fs, sh.tmpl.uni);
			sh.tmpl.trd = const_tex;
			for (k = 0; k < d->fs.npush && sh.tmpl.ntex < S8F_NTEX; k++) if (d->fs.push[k].type == S8S_PUSH_TEXTURE) { s8t_desc_make(&sh.tmpl.texd[sh.tmpl.ntex], 0x100000000ULL, 4, 4, S8T_FMT_RGBA8, S8T_LAYOUT_MORTON, 16); sh.tmpl.ntex++; }
			{ uint32_t si = 0; for (k = 0; k < d->fs.npush && si < S8F_NTEX; k++) if (d->fs.push[k].type == S8S_PUSH_SAMPLER) { uint32_t w2[2] = { agxrd(0, d->fs.push[k].va), agxrd(0, d->fs.push[k].va + 4) }; s8t_sampler_decode(&sh.tmpl.texs[si++], w2); } }
			sh.frag = fbase + fstart;
			sh.fraglen = (uint32_t)(flen - fstart > 0x1000 ? 0x1000 : flen - fstart);

			for (t = 0; t + 3 <= ntri; t += (d->prim == 6 ? 3 : 1)) {
				int q;

				for (q = 0; q < 3; q++) {
					uint32_t pos = t + (uint32_t)q, vid = pos;

					if (d->kind == S8S_DRAW_INDEXED) { uint32_t w = agxrd(0, (d->index_va + 2ULL * pos) & ~3ULL); vid = (pos & 1) ? (w >> 16) : (w & 0xffff); }
					idxv[q] = vid;
				}
				if (d->prim == 9 && (t & 1)) { uint32_t tmp = idxv[1]; idxv[1] = idxv[2]; idxv[2] = tmp; }
				for (q = 0; q < 3; q++) {
					uint32_t vid = idxv[q];
					struct s8f_ctx c;

					if (vid >= 1024 || vok[vid]) continue;
					memset(&c, 0, sizeof(c));
					memcpy(c.uni, uni, sizeof(c.uni));
					c.rd = agxrd; c.rdctx = 0; c.vid = vid;
					g_trace = (p == trace_pass && di == trace_draw && ok == 0 && fail == 0 && tried == 0);
					if (g_trace) printf("      TRACE vertex %u, uni0..11:", vid), ({ int z; for (z = 0; z < 12; z++) printf(" %08x", uni[z]); 0; }), printf("\n");
					vok[vid] = 2;
					tried++;
					if (!s8f_run(&c, vbase + vstart, (unsigned)(vlen - vstart > 0x400 ? 0x400 : vlen - vstart)) || (c.expmask & 0xf) != 0xf) {
						if (!fail) { fu = (uint32_t)c.unknown; fpc = c.pc_fail; fmask = c.expmask; }
						fail++;
						continue;
					}
					ok++;
					{
						f32b iw = sf_div(SF_ONE, c.exp[3]);
						float x, y;

						vt[vid].x = sf_fma(vp[1], sf_mul(c.exp[0], iw), vp[0]);
						vt[vid].y = sf_fma(vp[3], sf_mul(c.exp[1], iw), vp[2]);
						vt[vid].iw = iw;
						x = f32(vt[vid].x); y = f32(vt[vid].y);
						if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y;
						if (x >= -2 && x <= ps->width + 2 && y >= -2 && y <= ps->height + 2) inside++; else outside++;
						nvar = 0;
						while (nvar < S8R_MAXV && (c.expmask & (1u << (4 + nvar)))) { vt[vid].v[nvar] = c.exp[4 + nvar]; nvar++; }
						for (k = (uint32_t)nvar; k < S8R_MAXV; k++) vt[vid].v[k] = 0;
						vok[vid] = 1;
						if (verbose && ok <= 4) printf("      v%u: exp0..3 %g %g %g %g -> (%g, %g) iw %g, %d varyings\n", vid, f32(c.exp[0]), f32(c.exp[1]), f32(c.exp[2]), f32(c.exp[3]), x, y, f32(iw), nvar);
					}
				}
				if (idxv[0] < 1024 && idxv[1] < 1024 && idxv[2] < 1024 && vok[idxv[0]] == 1 && vok[idxv[1]] == 1 && vok[idxv[2]] == 1)
					s8r_tri(&vt[idxv[0]], &vt[idxv[1]], &vt[idxv[2]], nvar ? nvar : 1, 0, 0, sh.w, sh.h, emit, &sh);
			}
			if (fail == 0 && ok) nv_ok_draws++; else nv_fail_draws++;
			if (ok && outside == 0) onscreen_draws++;
			if (sh.stored) frag_store_draws++; else frag_nostore_draws++;
			if (fail) { int z = 1; uint32_t zi; for (zi = 0; zi < 16 && voff + zi < vlen; zi++) if (vbase[voff + zi]) z = 0; if (!z) unkv[fu & 255]++; }
			if (sh.unknown) unkf[sh.unknown & 255]++;
			printf("  draw %d %s prim %u n %u: vertices tried %u ok %u failed %u", di, d->kind == S8S_DRAW_INDEXED ? "idx" : "lst", d->prim, d->count, tried, ok, fail);
			if (fail) {
				int z = 1; uint32_t zi;

				for (zi = 0; zi < 16 && voff + zi < vlen; zi++) if (vbase[voff + zi]) z = 0;
				if (z) { printf(" [program bytes at the entry are ZERO in the capture]"); vzero_draws++; }
				else printf(" [first failure: unknown op 0x%02x at +%u, exports 0x%x, vs start %+d from entry]", fu, fpc, fmask, (int)vstart - (int)voff);
			}
			if (ok) printf("\n     positions x %g..%g y %g..%g (%d verts inside %ux%u, %d outside), %d varyings, vp %g %g %g %g uni0 %08x %08x", minx, maxx, miny, maxy, inside, ps->width, ps->height, outside, nvar, f32(vp[0]), f32(vp[1]), f32(vp[2]), f32(vp[3]), uni[0], uni[2]);
			printf("\n     fragment: start %s %+d from entry, %d textures, covered %u, shaded %u, stored %u", simg == 1 ? "block+0x40" : simg == 2 ? "walk-back" : simg == 3 ? "block+0x40(unverified)" : "entry(!)", (int)fstart - (int)foff, sh.tmpl.ntex, sh.cnt, sh.shaded, sh.stored);
			if (sh.stored) printf(", first at (%u,%u) rgba %g %g %g %g", sh.ex, sh.ey, f32(sh.out[0]), f32(sh.out[1]), f32(sh.out[2]), f32(sh.out[3]));
			else if (sh.shaded) printf(" -> NO STORE: %s (unknown op 0x%02x at +%u, discards %u, ran-off-end %u)", sh.unknown ? "unknown form" : sh.discard ? "discard" : "program ended without st_tile", sh.unknown, sh.first_fail_pc, sh.discard, sh.noend);
			else printf(" -> no pixel covered / no vertices");
			printf("\n");
		}
	}
	printf("\nSUMMARY: %d draws, %d run (%d skipped passes); vertex stage all-ok %d / failures %d; all vertices on screen %d; fragment stored %d / not stored %d\n", nd_total, nd_run, skipped, nv_ok_draws, nv_fail_draws, onscreen_draws, frag_store_draws, frag_nostore_draws);
	printf("vertex failures: %d draws with zero program bytes in the capture; others by unknown opcode:", vzero_draws);
	for (i = 0; i < 256; i++) if (unkv[i]) printf(" 0x%02x x%d", i, unkv[i]);
	printf("\nfragment failures by unknown opcode:");
	for (i = 0; i < 256; i++) if (unkf[i]) printf(" 0x%02x x%d", i, unkf[i]);
	printf("\n");
	return 0;
}
