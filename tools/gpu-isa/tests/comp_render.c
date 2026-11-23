/*
 * comp_render <snapshot dir> [extra dirs: program heaps / texel regions merged where a byte is zero] [-o out_prefix] [-all] [-v] [-synth] [-bg RRGGBBAA]
 *
 * HOST FRAME RENDERER: runs core/S8Agx.cpp.reference execComposite() logic on a capture snapshot (tests/replay_dump.py) at full speed. All live passes of the
 * kernel command list are rendered IN ORDER into real pass targets: every target VA gets its own linear BGRA8 buffer (width * 4 pitch, width/height from the
 * pass); later passes that sample a target by VA read that buffer (blur chains). The 1440x2960 target is the display surface (first use: opaque black). Same
 * per-draw logic as the kernel: vertex pre-pass (quad bbox), R8 layer textures (format 0x8002: linear, exactly as large as the quad), constant / destination-only
 * fast paths, premultiplied source-over for the pipeline blend bit. Texel data: whatever the snapshot holds at the texture VA (capture19 'type 9' regions);
 * regions that are zero or missing get a visible synthetic pattern (-synth forces the pattern everywhere).
 * Output: <prefix>.ppm of the display target (+ every target with -all); tests/ppm2png.py converts. Per pass / draw statistics on stdout.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s8gfx_frag.h"
#include "s8gfx_rast.h"
#include "s8gfx_stream.h"

struct reg { char name[32]; uint64_t va, len; uint8_t * m; };
static struct reg regs[320];
static int nregs;
struct tgt { uint64_t va; uint32_t w, h, pitch; uint8_t * m; uint64_t len; int display; uint32_t writes; };
static struct tgt tgts[32];
static int ntgts;
static int synth_all, no_selfblend;

#define SYN_BASE 0x7f0000000ULL

static struct reg * findreg(uint64_t va)
{
	int i;

	for (i = 0; i < nregs; i++) if (regs[i].va && va >= regs[i].va && va < regs[i].va + regs[i].len) return &regs[i];
	return 0;
}

#define TGT_BASE 0x7a0000000ULL
#define TGT_STEP 0x2000000ULL

/* pass targets are read (as textures) through an alias range, one 32 MB window per target: raw VAs never hit a target buffer, so the display surface's pixel span
 * (17 MB behind a 0x4000-byte resource) cannot shadow neighbouring resources */
static struct tgt * findtgt(uint64_t va)
{
	if (va >= TGT_BASE && va < TGT_BASE + TGT_STEP * 16) {
		int i = (int)((va - TGT_BASE) / TGT_STEP);

		if (i < ntgts) return &tgts[i];
	}
	return 0;
}
static int tgt_index(uint64_t va)
{
	int i;

	for (i = 0; i < ntgts; i++) if (va >= tgts[i].va && va < tgts[i].va + 0x10000) return i;   /* base of the target resource */
	return -1;
}

static int rd32(uint64_t va, uint32_t * out)
{
	struct tgt * t;
	struct reg * r;

	if (va >= SYN_BASE && va < SYN_BASE + 0x20000000ULL) {          /* synthetic texel pattern: 4096-byte rows, 16-pixel checker */
		uint64_t o = va - SYN_BASE;
		uint32_t y = (uint32_t)(o >> 12), v = 0, k;

		for (k = 0; k < 4; k++) {
			uint32_t x = (uint32_t)((o & 0xfff) + k);

			v |= (((x >> 4) ^ (y >> 4)) & 1 ? 0xd0u : 0x50u) << (8 * k);
		}
		*out = v;
		return 1;
	}
	t = findtgt(va);
	if (t) { uint64_t o = (va - TGT_BASE) % TGT_STEP; if (o + 4 <= (uint64_t)t->pitch * t->h) { memcpy(out, t->m + o, 4); return 1; } *out = 0; return 0; }
	r = findreg(va);
	if (!r || va + 4 > r->va + r->len) { *out = 0; return 0; }
	memcpy(out, r->m + (va - r->va), 4);
	return 1;
}

static int streamrd(void * ctx, uint64_t va, uint32_t * out) { (void)ctx; return rd32(va, out); }
static uint32_t agxrd(void * ctx, uint64_t va) { uint32_t v; (void)ctx; rd32(va, &v); return v; }

static int resregion(uint64_t va, const uint8_t ** base, uint64_t * off, uint64_t * len)
{
	struct reg * r = findreg(va);

	if (!r) return 0;
	*base = r->m; *off = va - r->va; *len = r->len;
	return 1;
}

static float f32(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

static void bind_uniforms(const struct s8s_prog * pg, uint32_t * uni)
{
	uint32_t k, j;

	for (k = 0; k < pg->npush; k++) {
		const struct s8s_push * pu = &pg->push[k];
		uint32_t halves;

		if (pu->type != 0x03 && pu->type != 0x33) continue;
		halves = pu->halves ? pu->halves : 32;
		for (j = 0; j < halves / 2 && pu->dest + 2 * j < S8F_NUNI; j++) uni[pu->dest + 2 * j] = agxrd(0, pu->va + 4 * j);
	}
}

/* texel memory present in the snapshot? (first 4 KB of the texture not all zero) */
static int texels_present(uint64_t va)
{
	uint32_t k, v;

	if (synth_all) return 0;
	for (k = 0; k < 0x11000; k++) { if (!rd32(va + 4 * k, &v)) break; if (v) return 1; }      /* the first rows of a layer texture are often transparent: scan up to 272 KB */
	return 0;
}

struct comp_px {
	struct s8f_ctx tmpl, work, vwork;
	const uint8_t * frag;
	uint32_t fraglen;
	uint8_t * target;
	uint32_t pitch;
	int w, h, blend;
	uint32_t pixels, unknown, stored, noend, unkpc, firstOut, lastOut, rawOut[4];
	int constant, tileOnly, cacheValid;
	uint32_t cacheTile, cacheCb, cacheCg, cacheCr, cacheCa, cb, cg, cr, ca;
	uint32_t texw[S8F_NTEX][6], smpw[S8F_NTEX][2];
	uint32_t synth[S8F_NTEX];
};
static struct comp_px dcs;

static void comp_emit(void * vctx, int px, int py, const f32b * v)
{
	struct comp_px * dc = (struct comp_px *)vctx;
	struct s8f_ctx * fc = &dc->work;
	uint32_t * dst;
	uint32_t sb = 0, sg = 0, sr = 0, sa = 0, dv;
	int have = 0, selfblend = 0;

	if (px < 0 || py < 0 || px >= dc->w || py >= dc->h) return;
	dst = (uint32_t *)(dc->target + (uint64_t)py * dc->pitch + (uint64_t)px * 4);
	dv = *dst;
	dc->pixels++;
	if (dc->constant) { sb = dc->cb; sg = dc->cg; sr = dc->cr; sa = dc->ca; have = 1; }
	else if (dc->tileOnly && dc->cacheValid && dc->cacheTile == dv) { sb = dc->cacheCb; sg = dc->cacheCg; sr = dc->cacheCr; sa = dc->cacheCa; have = 1; }
	else {
		memcpy(fc, &dc->tmpl, sizeof(*fc));
		fc->varying = v;
		fc->tile = dv;
		fc->has_tile = 1;
		fc->sr[48] = (uint32_t)px & 31;
		fc->sr[49] = (uint32_t)py & 31;
		fc->sr[16] = (uint32_t)px;
		fc->sr[17] = (uint32_t)py;
		if (s8f_run(fc, dc->frag, dc->fraglen) && fc->stored) {
			sb = sf_to_unorm8(fc->out[2]); sg = sf_to_unorm8(fc->out[1]); sr = sf_to_unorm8(fc->out[0]); sa = sf_to_unorm8(fc->out[3]);
			have = 1;
			if (fc->hit[0x79] || fc->hit[0xf9]) selfblend = 1;
			if (dc->tileOnly) { dc->cacheTile = dv; dc->cacheCb = sb; dc->cacheCg = sg; dc->cacheCr = sr; dc->cacheCa = sa; dc->cacheValid = 1; }
		} else if (fc->unknown != 0) { dc->unknown = (uint32_t)fc->unknown; dc->unkpc = fc->pc_fail; }
		else dc->noend++;
	}
	if (!have) return;
	if (dc->blend && !(selfblend && !no_selfblend)) {      /* programs that read the destination (ld_tile) blend themselves: no second hardware blend (-selfblend to force) */
		uint32_t d = dv, ia = 255 - sa;

		sb = sb + (((d & 0xff) * ia + 127) / 255);
		sg = sg + ((((d >> 8) & 0xff) * ia + 127) / 255);
		sr = sr + ((((d >> 16) & 0xff) * ia + 127) / 255);
		sa = sa + ((((d >> 24) & 0xff) * ia + 127) / 255);
		if (sb > 255) sb = 255; if (sg > 255) sg = 255; if (sr > 255) sr = 255; if (sa > 255) sa = 255;
	}
	*dst = sb | (sg << 8) | (sr << 16) | (sa << 24);
	if (dc->stored == 0) { dc->firstOut = *dst; dc->rawOut[0] = sb; dc->rawOut[1] = sg; dc->rawOut[2] = sr; dc->rawOut[3] = sa; }
	dc->lastOut = *dst;
	dc->stored++;
}

static int comp_vertex(struct s8f_ctx * c, uint32_t vid, const uint8_t * code, unsigned codelen, const uint32_t * uni, const uint32_t * vp, struct s8r_vtx * out, uint32_t * nvar)
{
	f32b iw;

	memset(c, 0, sizeof(*c));
	memcpy(c->uni, uni, sizeof(c->uni));
	c->rd = agxrd;
	c->rdctx = 0;
	c->vid = vid;
	if (!s8f_run(c, code, codelen) || (c->expmask & 0xf) != 0xf) return 0;
	iw = sf_div(SF_ONE, c->exp[3]);
	out->x = sf_fma(vp[1], sf_mul(c->exp[0], iw), vp[0]);
	out->y = sf_fma(vp[3], sf_mul(c->exp[1], iw), vp[2]);
	out->iw = iw;
	*nvar = 0;
	while (*nvar < S8R_MAXV && (c->expmask & (1u << (4 + *nvar)))) { out->v[*nvar] = c->exp[4 + *nvar]; (*nvar)++; }
	{ uint32_t k; for (k = *nvar; k < S8R_MAXV; k++) out->v[k] = 0; }
	return 1;
}

static int prog_reads_tile(const uint8_t * m, unsigned len)
{
	unsigned pc = 0;

	while (pc + 2 <= len) {
		unsigned n = s8f_len(m + pc, len - pc);

		if (m[pc] == 0xf9) n = 8;
		if (n == 0) break;
		if (m[pc] == 0x79 || m[pc] == 0xf9) return 1;
		if (m[pc] == 0x08 && m[pc + 1] == 0x01) break;
		pc += n;
	}
	return 0;
}

static int keep_full;

static void write_ppm(const char * path, struct tgt * t)
{
	FILE * f = fopen(path, "wb");
	uint32_t x, y;

	if (!f) return;
	fprintf(f, "P6\n%u %u\n255\n", t->w, t->h);
	for (y = 0; y < t->h; y++) {
		for (x = 0; x < t->w; x++) {
			uint32_t v = *(uint32_t *)(t->m + (uint64_t)y * t->pitch + 4 * (uint64_t)x);
			uint32_t a = v >> 24, r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
			uint8_t px[3];

			if (!t->display) {      /* offscreen layers: show over a mid-grey checker so transparency is visible */
				uint32_t bg = (((x >> 3) ^ (y >> 3)) & 1) ? 90 : 130;

				r = (r * 255 + bg * (255 - a)) / 255 > 255 ? 255 : (r + (bg * (255 - a) + 127) / 255);
				g = g + (bg * (255 - a) + 127) / 255; b = b + (bg * (255 - a) + 127) / 255;
				if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
			}
			px[0] = (uint8_t)r; px[1] = (uint8_t)g; px[2] = (uint8_t)b;
			fwrite(px, 1, 3, f);
		}
	}
	fclose(f);
}

int main(int argc, char ** argv)
{
	char path[512], line[256];
	const char * prefix = "/tmp/frame";
	FILE * f;
	static struct s8s_pass passes[32];
	static struct s8s_draw draws[512];
	static struct s8r_vtx vt[1024];
	static uint8_t vok[1024];
	uint8_t * cl = 0;
	long cllen = 0;
	int i, np, p, verbose = 0, all = 0;
	int nd_total = 0, nd_run = 0, st_draws = 0, nostore = 0, skipped = 0, nofrag = 0, nvfail = 0;
	static int unkf[256], unkv[256];
	uint32_t bgc = 0xff000000u;

	if (argc < 2) { fprintf(stderr, "usage: comp_render <dir> [extra dirs] [-o prefix] [-all] [-v] [-synth] [-bg AARRGGBB]\n"); return 2; }
	for (i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) verbose = 1;
		else if (!strcmp(argv[i], "-all")) all = 1;
		else if (!strcmp(argv[i], "-synth")) synth_all = 1;
		else if (!strcmp(argv[i], "-selfblend")) no_selfblend = 1;
		else if (!strcmp(argv[i], "-keepfull")) keep_full = 1;
		else if (!strcmp(argv[i], "-o") && i + 1 < argc) prefix = argv[++i];
		else if (!strcmp(argv[i], "-bg") && i + 1 < argc) bgc = (uint32_t)strtoul(argv[++i], 0, 16);
	}
	snprintf(path, sizeof(path), "%s/regions.txt", argv[1]);
	f = fopen(path, "r");
	if (!f) { fprintf(stderr, "no %s\n", path); return 2; }
	while (fgets(line, sizeof(line), f) && nregs < 300) {
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
	for (i = 2; i < argc; i++) {     /* program heaps / texel regions from other snapshots fill zero bytes */
		if (argv[i][0] == '-') continue;
		if (i > 2 && (!strcmp(argv[i - 1], "-o") || !strcmp(argv[i - 1], "-bg"))) continue;
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
			if (!(va == 0x100020000ULL || va == 0x1000e0000ULL || va == 0x100610000ULL || (va >= 0x1006b0000ULL && va < 0x100890000ULL) || (va >= 0x100900000ULL && strncmp(nm, "res_", 4) == 0 && 0))) continue;
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
			if (!found && nregs < 319) { snprintf(regs[nregs].name, 32, "%s", nm); regs[nregs].va = va; regs[nregs].len = len; regs[nregs].m = buf; nregs++; }
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
		struct tgt * tg;
		int nd, di, k;

		if (!streamrd(0, ps->stream, &first) || first != 0x80000001u || ps->width == 0 || ps->height == 0 || ps->width > 4096 || ps->height > 4096) { printf("== pass %d skipped (stale block)\n", p); skipped++; continue; }
		s8s_pass_target(streamrd, 0, ps);
		if (!ps->has_target) { printf("== pass %d skipped (no target)\n", p); skipped++; continue; }
		tg = 0;
		for (k = 0; k < ntgts; k++) if (tgts[k].va == ps->target_va) tg = &tgts[k];
		if (!tg || tg->w * 4ull * tg->h < (uint64_t)ps->width * 4 * ps->height) {
			uint64_t need = (uint64_t)ps->width * 4 * ps->height, cap = need < (uint64_t)1440 * 4 * 2960 ? (uint64_t)1440 * 4 * 2960 : need;

			if (!tg) { tg = &tgts[ntgts++]; tg->va = ps->target_va; tg->m = (uint8_t *)calloc(1, cap); tg->len = cap; tg->display = (ps->width == 1440 && ps->height == 2960); }
			tg->w = ps->width; tg->h = ps->height; tg->pitch = ps->width * 4;
			if (tg->display) { uint32_t * q = (uint32_t *)tg->m; uint64_t n; for (n = 0; n < cap / 4; n++) q[n] = bgc; }
		} else { tg->w = ps->width; tg->h = ps->height; tg->pitch = ps->width * 4; }
		nd = s8s_scan(streamrd, 0, ps->stream, (uint32_t)p, draws, 512, &st);
		printf("== pass %d %ux%u target %llx%s, %d draws\n", p, ps->width, ps->height, (unsigned long long)ps->target_va, tg->display ? " (display)" : "", nd);
		for (di = 0; di < nd; di++) {
			struct s8s_draw * d = &draws[di];
			const uint8_t * vbase, * fbase;
			uint64_t voff, vlen, foff, flen;
			unsigned vstart, fstart = 0;
			uint32_t vp[4], uni[S8F_NUNI], nvar = 0, nvid, vid, t, kk, idxv[3], dTried = 0, dOk = 0, dFail = 0, dUnk = 0;
			int32_t bx0 = 0, bx1 = 0, by0 = 0, by1 = 0;
			int bbox = 0, simg = 0;
			struct comp_px * dc = &dcs;

			nd_total++;
			if (d->kind == S8S_DRAW_OTHER || d->count < 3 || d->count > 4000 || (d->prim != 6 && d->prim != 9)) { printf("  draw %d: skipped (kind %u prim %u count %u)\n", di, d->kind, d->prim, d->count); continue; }
			if (!resregion(d->vs.entry, &vbase, &voff, &vlen) || !resregion(d->fs.entry, &fbase, &foff, &flen)) { printf("  draw %d: program region not in the capture (vs %llx fs %llx)\n", di, (unsigned long long)d->vs.entry, (unsigned long long)d->fs.entry); nofrag++; continue; }
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
			memset(dc, 0, sizeof(*dc));
			memset(uni, 0, sizeof(uni));
			bind_uniforms(&d->vs, uni);
			bind_uniforms(&d->fs, dc->tmpl.uni);
			dc->tmpl.rd = 0; dc->tmpl.fetch = 0;
			dc->tmpl.trd = agxrd;
			memset(vok, 0, sizeof(vok));
			{   uint32_t tmp; if (d->kind == S8S_DRAW_INDEXED && !rd32(d->index_va, &tmp)) { printf("  draw %d: INDEX DATA NOT IN THE CAPTURE (index buffer %llx; the vertex/index/uniform ring is not dumped: dump res at 0x100a70000 and 0x1009d0000)\n", di, (unsigned long long)d->index_va); nofrag++; continue; } }
			/* vertex pre-pass */
			nvid = d->count;
			if (d->kind == S8S_DRAW_INDEXED) {
				nvid = 0;
				for (t = 0; t < d->count; t++) { uint32_t w = agxrd(0, (d->index_va + 2ULL * t) & ~3ULL), ix = (t & 1) ? (w >> 16) : (w & 0xffff); if (ix + 1 > nvid) nvid = ix + 1; }
			}
			if (nvid > 1024) nvid = 1024;
			for (vid = 0; vid < nvid; vid++) {
				uint32_t nv = 0;

				vok[vid] = 2; dTried++;
				if (!comp_vertex(&dc->vwork, vid, vbase + vstart, (unsigned)(vlen - vstart > 0x400 ? 0x400 : vlen - vstart), uni, vp, &vt[vid], &nv)) { if (!dFail) { dUnk = (uint32_t)dc->vwork.unknown; dFail = 1; } continue; }
				nvar = nv; dOk++;
				{
					int32_t fx = sf_to_fixed(vt[vid].x, 0), fy = sf_to_fixed(vt[vid].y, 0);

					if (!bbox || fx < bx0) bx0 = fx; if (!bbox || fx > bx1) bx1 = fx; if (!bbox || fy < by0) by0 = fy; if (!bbox || fy > by1) by1 = fy;
					bbox = 1;
				}
				vok[vid] = 1;
			}
			/* textures */
			for (kk = 0; kk < d->fs.npush && dc->tmpl.ntex < S8F_NTEX; kk++) {
				if (d->fs.push[kk].type == S8S_PUSH_TEXTURE) {
					uint32_t w6[6], ti = (uint32_t)dc->tmpl.ntex;
					uint64_t tva = 0;
					int j, ok;
					struct tgt * st2;
					int form;

					for (j = 0; j < 6; j++) w6[j] = agxrd(0, d->fs.push[kk].va + 4 * j);
					memcpy(dc->texw[ti], w6, sizeof(w6));
					if (getenv("TEXDUMP")) printf("  TEXREC pass %d draw %d tex %u: %08x %08x %08x %08x %08x %08x\n", p, di, ti, w6[0], w6[1], w6[2], w6[3], w6[4], w6[5]);
					ok = s8t_desc_decode(&dc->tmpl.texd[ti], w6);
					form = s8s_texrec_va(w6, &tva);
					if (!form) tva = dc->tmpl.texd[ti].va;
					st2 = 0;
					if ((w6[0] & 0xffff) == 0x8002 && form) {
						uint32_t tw = bbox ? (uint32_t)(bx1 - bx0) : ps->width, th = bbox ? (uint32_t)(by1 - by0) : ps->height;

						if (synth_all || !texels_present(tva)) { s8t_desc_make(&dc->tmpl.texd[ti], SYN_BASE + 0x1000000ULL * ti, tw, th, S8T_FMT_A8, S8T_LAYOUT_LINEAR, 4096); dc->synth[ti] = 1; }
						else {
							/* capture21: the texel buffer starts with a 0x40-byte header {2, 2, 0x40, 9, pitch, height, width, height, 1,...}; texels at +0x40, rows are `pitch` bytes (704 for a 651-wide mask) */
							uint32_t h2 = 0, pitch = tw;

							if (rd32(tva - 0x40 + 8, &h2) && h2 == 0x40) {
								uint32_t hp = 0, hh = 0, hw = 0;

								rd32(tva - 0x40 + 16, &hp); rd32(tva - 0x40 + 20, &hh); rd32(tva - 0x40 + 24, &hw);
								if (hw >= 1 && hw < 8192 && hh >= 1 && hh < 8192) { tw = hp >= hw ? hp : hw; th = hh; pitch = (tw + 63) & ~63u;     /* sampler width = header dword [4] (704 for the clock whose visible width is 651: normalised u spans the padded width), pitch = that aligned to 64 */ }      /* row stride = width aligned to 64 bytes: 651 -> 704, 694 -> 704 (the header word [4] says 694 for the date, so it is NOT the pitch) */      /* header {.., pitch, height, width, height}: the real texture size (651x387 for a quad that is 582 wide) */
							}
							s8t_desc_make(&dc->tmpl.texd[ti], tva, tw, th, S8T_FMT_A8, S8T_LAYOUT_LINEAR, pitch);
						}
					} else if (tgt_index(tva) >= 0 && !synth_all) {
						int ti2 = tgt_index(tva);

						st2 = &tgts[ti2];
						/* a previous pass's target: linear BGRA8 with that target's pitch (the kernel's Morton + pass-size guess is wrong for these) */
						s8t_desc_make(&dc->tmpl.texd[ti], TGT_BASE + TGT_STEP * (uint64_t)ti2 + (tva - st2->va), st2->w, st2->h, S8T_FMT_BGRA8, S8T_LAYOUT_LINEAR, st2->pitch);
					} else if (!ok && form == 2) {
						/* a real texture record: size / format / layout come from the descriptor (s8gfx_tex.h, analysis/texture-state.md sec. 9) */
						struct s8t_desc * td = &dc->tmpl.texd[ti];

						if (synth_all || !texels_present(tva)) { s8t_desc_make(td, SYN_BASE + 0x1000000ULL * ti, td->width, td->height, td->fmt, S8T_LAYOUT_LINEAR, td->width * s8t_bpp(td->fmt)); dc->synth[ti] = 1; }
						else td->va = tva;
					} else if (form || !ok) {
						uint32_t tw = bbox ? (uint32_t)(bx1 - bx0) : ps->width, th = bbox ? (uint32_t)(by1 - by0) : ps->height;

						if (tw == 0) tw = 1; if (th == 0) th = 1;
						if (synth_all || !texels_present(tva)) { s8t_desc_make(&dc->tmpl.texd[ti], SYN_BASE + 0x1000000ULL * ti, tw, th, S8T_FMT_BGRA8, S8T_LAYOUT_LINEAR, 16384); dc->synth[ti] = 1; }
						else s8t_desc_make(&dc->tmpl.texd[ti], tva, tw, th, S8T_FMT_BGRA8, S8T_LAYOUT_LINEAR, tw * 4);
					}
					dc->tmpl.ntex++;
				}
			}
			{ uint32_t si = 0; for (kk = 0; kk < d->fs.npush && si < S8F_NTEX; kk++) if (d->fs.push[kk].type == S8S_PUSH_SAMPLER) { uint32_t w2[2] = { agxrd(0, d->fs.push[kk].va), agxrd(0, d->fs.push[kk].va + 4) }; dc->smpw[si][0] = w2[0]; dc->smpw[si][1] = w2[1]; s8t_sampler_decode(&dc->tmpl.texs[si++], w2); } }
			dc->frag = fbase + fstart;
			dc->fraglen = (uint32_t)(flen - fstart > 0x4000 ? 0x4000 : flen - fstart);      /* 0x4000 (was 0x1000): the camera-glass fragment program is longer than 4 KB */
			dc->target = tg->m; dc->pitch = tg->pitch; dc->w = (int)ps->width; dc->h = (int)ps->height; dc->blend = (int)d->blend;
			/* constant-output probe (same as the kernel) */
			if (dc->tmpl.ntex == 0) {
				f32b pv[2][S8R_MAXV];
				uint32_t po[3][4];
				int pok = 1, a, k;
				static const uint32_t ptile[3] = { 0x80402010u, 0x80402010u, 0u };

				for (a = 0; a < 2; a++) for (k = 0; k < S8R_MAXV; k++) pv[a][k] = sf_from_i64((int64_t)(a * 3 + k));
				for (a = 0; a < 3 && pok; a++) {
					int vsel = a == 1 ? 1 : 0;

					memcpy(&dc->work, &dc->tmpl, sizeof(dc->work));
					dc->work.varying = pv[vsel]; dc->work.tile = ptile[a]; dc->work.has_tile = 1;
					dc->work.sr[48] = a == 1 ? 9u : 0u; dc->work.sr[49] = a == 1 ? 13u : 0u; dc->work.sr[16] = a == 1 ? 101u : 0u; dc->work.sr[17] = a == 1 ? 77u : 0u;
					if (!s8f_run(&dc->work, dc->frag, dc->fraglen) || !dc->work.stored) { pok = 0; break; }
					for (k = 0; k < 4; k++) po[a][k] = dc->work.out[k];
				}
				if (pok) {
					int eq01 = 1, eq02 = 1;

					for (k = 0; k < 4; k++) { eq01 = eq01 && po[0][k] == po[1][k]; eq02 = eq02 && po[0][k] == po[2][k]; }
					if (eq01 && eq02) { dc->constant = 1; dc->cb = sf_to_unorm8(po[0][2]); dc->cg = sf_to_unorm8(po[0][1]); dc->cr = sf_to_unorm8(po[0][0]); dc->ca = sf_to_unorm8(po[0][3]); }
					else if (eq01) dc->tileOnly = 1;
				}
			}
			if (!keep_full && tg->display && bbox && (bx1 - bx0) >= (int)ps->width - 2 && (by1 - by0) >= (int)ps->height - 2 && dc->blend && prog_reads_tile(dc->frag, dc->fraglen)) {
				printf("  draw %d: SKIPPED full-screen destination-reading pass (dither / resolve style, f16 forms not decoded yet; -keepfull renders it): vs %llx fs %llx\n", di, (unsigned long long)d->vs.entry, (unsigned long long)d->fs.entry);
				continue;
			}
			for (t = 0; t + 3 <= d->count; t += (d->prim == 6 ? 3 : 1)) {
				int q;

				for (q = 0; q < 3; q++) {
					uint32_t pos = t + (uint32_t)q, v2 = pos;

					if (d->kind == S8S_DRAW_INDEXED) { uint32_t w = agxrd(0, (d->index_va + 2ULL * pos) & ~3ULL); v2 = (pos & 1) ? (w >> 16) : (w & 0xffff); }
					idxv[q] = v2;
				}
				if (d->prim == 9 && (t & 1)) { uint32_t tmp = idxv[1]; idxv[1] = idxv[2]; idxv[2] = tmp; }
				for (q = 0; q < 3; q++) {
					uint32_t v2 = idxv[q], nv = 0;

					if (v2 >= 1024 || vok[v2]) continue;
					vok[v2] = 2; dTried++;
					if (!comp_vertex(&dc->vwork, v2, vbase + vstart, (unsigned)(vlen - vstart > 0x400 ? 0x400 : vlen - vstart), uni, vp, &vt[v2], &nv)) { if (!dFail) { dUnk = (uint32_t)dc->vwork.unknown; dFail = 1; } continue; }
					nvar = nv; dOk++; vok[v2] = 1;
				}
				if (idxv[0] < 1024 && idxv[1] < 1024 && idxv[2] < 1024 && vok[idxv[0]] == 1 && vok[idxv[1]] == 1 && vok[idxv[2]] == 1)
					s8r_tri(&vt[idxv[0]], &vt[idxv[1]], &vt[idxv[2]], (int)(nvar ? nvar : 1), 0, 0, dc->w, dc->h, comp_emit, dc);
			}
			if (dFail && !dOk) { nvfail++; unkv[dUnk & 255]++; }
			if (dc->stored) st_draws++; else { nostore++; if (dc->unknown) unkf[dc->unknown & 255]++; }
			tg->writes += dc->stored;
			if (tg->display) {
				uint64_t nz = 0, n; uint32_t * q = (uint32_t *)tg->m;

				for (n = 0; n < (uint64_t)tg->w * tg->h; n++) if (q[n] & 0xffffff) nz++;
				printf("  [display target after this draw: %llu non-black pixels]\n", (unsigned long long)nz);
			}
			printf("  draw %d %s prim %u n %u: vs %llx fs %llx(%s%+d) tex %d%s%s%s blend %d bbox (%d,%d)-(%d,%d) vtx %u/%u | covered %u stored %u%s", di,
			    d->kind == S8S_DRAW_INDEXED ? "idx" : "lst", d->prim, d->count, (unsigned long long)d->vs.entry, (unsigned long long)d->fs.entry, simg == 1 ? "blk" : simg == 2 ? "walk" : simg == 3 ? "unv" : "ent",
			    (int)fstart - (int)foff, dc->tmpl.ntex, dc->synth[0] ? " (synthetic texels)" : "", dc->constant ? " CONST" : dc->tileOnly ? " TILEONLY" : "", "", dc->blend,
			    bx0, by0, bx1, by1, dOk, dTried, dc->pixels, dc->stored, dc->stored ? "" : " NOT STORED");
			if (dc->stored) printf(" out first %08x last %08x (blended b,g,r,a %u %u %u %u)", dc->firstOut, dc->lastOut, dc->rawOut[0], dc->rawOut[1], dc->rawOut[2], dc->rawOut[3]);
			if (!dc->stored && dc->pixels) printf(" [%s op 0x%02x at +%u]", dc->unknown ? "unknown" : "program ended without st_tile", dc->unknown, dc->unkpc);
			if (dFail) printf(" [vertex failures: first op 0x%02x]", dUnk);
			printf("\n");
			if (verbose) {
				for (kk = 0; kk < dc->tmpl.ntex; kk++) printf("      tex%u %08x %08x %08x %08x -> va %llx %ux%u fmt %u pitch %u smp %08x %08x\n", kk, dc->texw[kk][0], dc->texw[kk][1], dc->texw[kk][2], dc->texw[kk][3],
				    (unsigned long long)dc->tmpl.texd[kk].va, dc->tmpl.texd[kk].width, dc->tmpl.texd[kk].height, dc->tmpl.texd[kk].fmt, dc->tmpl.texd[kk].pitch, dc->smpw[kk][0], dc->smpw[kk][1]);
			}
			(void)f32;
		}
	}
	printf("\nSUMMARY: %d draws, %d run, %d passes skipped, %d draws without program region; fragment stored %d draws / nothing stored %d; draws with all vertices failing %d\n", nd_total, nd_run, skipped, nofrag, st_draws, nostore, nvfail);
	printf("fragment unknown ops (draws):");
	for (i = 0; i < 256; i++) if (unkf[i]) printf(" 0x%02x x%d", i, unkf[i]);
	printf("\nvertex unknown ops (draws, all vertices failed):");
	for (i = 0; i < 256; i++) if (unkv[i]) printf(" 0x%02x x%d", i, unkv[i]);
	printf("\n");
	{
		struct tgt * best = 0;

		for (i = 0; i < ntgts; i++) {
			if (tgts[i].display && (!best || tgts[i].writes >= best->writes)) best = &tgts[i];
		}
		if (best) { snprintf(path, sizeof(path), "%s.ppm", prefix); write_ppm(path, best); printf("display target %llx (%llu pixels stored in total) -> %s\n", (unsigned long long)best->va, (unsigned long long)best->writes, path); }
		if (all) for (i = 0; i < ntgts; i++) { snprintf(path, sizeof(path), "%s_t%llx.ppm", prefix, (unsigned long long)tgts[i].va); write_ppm(path, &tgts[i]); printf("target %llx %ux%u written %u px -> %s\n", (unsigned long long)tgts[i].va, tgts[i].w, tgts[i].h, tgts[i].writes, path); }
	}
	return 0;
}
