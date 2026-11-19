/*
 * Decoder for the compositor's (backboardd / CoreAnimation) AGX command stream: turns a pass's stream into a DRAW LIST.
 * Format, evidence and confidence per field: analysis/compositor-stream.md.  Host test: tests/stream_decode.c (capture17).
 * Integer-only, no libc, C++-compatible (only <stdint.h>). Memory is read through a callback, like core/S8Agx.cpp.reference agxRd,
 * but with an explicit "mapped" result so that zero words are distinguishable from holes:
 *
 *     static int rd(void *ctx, uint64_t va, uint32_t *out) { ... *out = word at GPU VA; return 1 if va is inside a resource; ... }
 *
 * All pointers inside the stream are 32-bit and mean GPU VA 0x1_0000_0000 | lo32 (every backboardd resource lives below 0x2_0000_0000).
 *
 * Stream grammar (dwords, little endian):
 *   80000001 <lo>            LINK    continue at VA lo
 *   c0000000                 END     of the pass stream
 *   (n<<8)|1 <lo>            LOAD    copy n dwords from VA lo into the hardware state; the kind is told by n and the first dword:
 *        n=9  c00 ..              viewport block   [0xc00][0x8000_0000|xmin<<16|xmax][ymin<<16|ymax][tx][sx][ty][sy][0.0][1.0]  (32-px tile bounds)
 *        n=6  1f ..               pipeline state   [0x1f][..][ctl][0x0e000000][ctl][0x0e000000], ctl bit 24 = blend (HYPOTHESIS)
 *        n=5  c0 ..               fragment bind    [0xc0][flags][fragment slot VA|k][program tail VA][reg-count class]
 *        n=3  02000000 ..         end-of-pass tile bounds        n=3 00210000 ..  constant       n=3 other: sampler state
 *        n=2  00020000 0N0F0000   vertex info (N = vertex register class = B value)        n=2 00000020 x  draw/visibility slot
 *   48004000|48002000 <w>   A  vertex-stage launch (low half 0x4000 / 0x2000 = thread capacity class), followed by
 *   <slot VA|4> <regs>      B  pointer to the vertex slot in the vertex state ring (res_16 in capture17) and its register class
 *   5000xxxx 0000ffff       index prefix (before every indexed draw; 0xc060 before the 7e44 draws)
 *   6e44pp00 count inst-1 0 trailer          non-indexed draw  (pp = primitive: 1 = lines? 6 = triangle list, 9 = triangle strip)
 *   7e42pp01 idxVA count inst-1 0 trailer    indexed draw      (16-bit indices, ASSUMED)
 *   7e440001 ptr 0 0 0 trailer               "other" draw: lives in the chunk right after a pass's head LINK, which the LINK skips (the link is
 *                                            patched past it when the pass needs no clear? HYPOTHESIS); the chunk ends with 20000160 and falls into the body
 *   20000160                                 single-word token between that chunk and the body
 * A slot (vertex or fragment) is a run of 2-dword push records followed by one state record:
 *   push  <w0> <w1>:  type = w0 & 0xff: 0x03 / 0x33 uniform, 0x13 sampler state, 0x17 texture state;
 *                     source VA = (w1 << 8) | (w0 >> 24 & 0xfc), size = (w0 >> 20 & 0x1f) halves (0 = 32), destination = w0 >> 8 & 0xff
 *   state <hdr> <entry|flags> [extra if hdr bit 10] [0x108]  [c0400038 <program block VA>]:
 *                     hdr 0x0001xx1f/2f/3f = vertex program, 0x0011xx1f/2f/3f = fragment program
 */
#ifndef S8GFX_STREAM_H
#define S8GFX_STREAM_H

#include <stdint.h>

#define S8S_HI           0x100000000ULL
#define S8S_MAXPUSH      16
#define S8S_MAXDRAW      512

#define S8S_DRAW_LIST    0       /* 6e44: non-indexed */
#define S8S_DRAW_INDEXED 1       /* 7e42 */
#define S8S_DRAW_OTHER   2       /* 7e44 */

#define S8S_PUSH_UNIFORM 0x03
#define S8S_PUSH_SAMPLER 0x13
#define S8S_PUSH_TEXTURE 0x17

typedef int (*s8s_rd_fn)(void * ctx, uint64_t va, uint32_t * out);

struct s8s_push {
	uint32_t w0, w1;
	uint32_t type;                  /* w0 & 0xff */
	uint64_t va;                    /* source address */
	uint32_t halves;                /* size in 16-bit units (uniform pushes; 32 when the field is 0) */
	uint32_t dest;                  /* destination register / state index (w0 >> 8 & 0xff) */
	uint32_t flags;                 /* (w0 >> 24) & 3: bit 1 seen set on uniform pushes whose data is f32, meaning [?] */
};

struct s8s_prog {
	uint32_t found;                 /* a state record was found */
	uint64_t slot;                  /* slot start VA (pointer & ~0x1f) */
	uint32_t slot_k;                /* pointer & 0x1f */
	uint32_t hdr;                   /* state header dword */
	uint64_t entry;                 /* program entry VA (pointer & ~3) */
	uint32_t entry_flags;           /* pointer & 3 */
	uint64_t extra;                 /* second pointer when hdr bit 10 is set (0x2cf58 / 0x2f358 class), else 0 */
	uint64_t block;                 /* c0400038 record: start of the program image (header block), else 0 */
	uint32_t npush;
	struct s8s_push push[S8S_MAXPUSH];
};

struct s8s_draw {
	uint32_t pass;                  /* index of the pass in the call */
	uint64_t pc;                    /* VA of the draw header in the stream */
	uint32_t kind;                  /* S8S_DRAW_* */
	uint32_t hdr;                   /* draw header dword */
	uint32_t prim;                  /* primitive code = hdr >> 8 & 0xff */
	uint32_t count;                 /* vertices (or indices) */
	uint32_t instances;
	uint64_t index_va;              /* indexed draws */
	uint32_t index_mask;            /* 0000ffff word of the index prefix */
	uint32_t trailer;               /* 0xaa / 0x1fe / 0x100 / 0x200: not decoded */
	uint32_t launch0, launch1;      /* A record: 0x48004000 / 0x48002000, 2 / 4 */
	uint32_t vs_regs;               /* B value */
	uint32_t vsinfo[2];             /* 00020000 0N0F0000 */
	uint32_t fs_bind[5];            /* LOAD5 */
	uint32_t fs_regs;               /* LOAD5 last dword */
	uint64_t fs_tail;               /* LOAD5 program tail VA */
	uint32_t pipe[6];
	uint32_t blend;                 /* pipe[2] bit 24 (HYPOTHESIS) */
	uint32_t sampler[3];            /* LOAD3 from the sampler ring */
	uint64_t sampler_va;
	uint32_t drawslot[2];           /* LOAD2 00000020 x */
	uint32_t vp_valid;
	uint32_t vp[4];                 /* float bits: translate x, scale x, translate y, scale y (centre / half-extent, y flipped) */
	uint32_t tile_x0, tile_x1, tile_y0, tile_y1;   /* 32-pixel tile bounds of the pass region (viewport block words 1 and 2) */
	struct s8s_prog vs, fs;
};

struct s8s_stats {
	uint32_t tokens, loads, links, draws, unknown_words, unknown_loads, end_seen, bad_ptr, barriers;
	uint32_t first_unknown;
};

struct s8s_pass {
	uint32_t block;                 /* byte offset of the block in the kernel command list */
	uint64_t stream;                /* stream start VA (a LINK) */
	uint32_t width, height;         /* kernel command list block +0x690 / +0x694 */
	uint32_t slotA, slotB;          /* fragment slot pointers (block +0x2b0, +0x2d0): load/clear and store programs of the pass */
	uint32_t flags;                 /* block +0x2a8 */
	uint32_t has_target;
	uint64_t target_va;             /* render target taken from the first texture-state push of the two slots */
	uint32_t target_fmt, target_w1; /* its first two dwords */
	uint32_t target_form;           /* 1: {fmt, w1, VA>>4, 0x200..}   2: {fmt, w1, 0x40000000 | VA>>2, ..} */
};

static inline uint32_t
s8s_u32(s8s_rd_fn rd, void * ctx, uint64_t va, int * ok)
{
	uint32_t v = 0;

	if (!rd(ctx, va, &v)) {
		*ok = 0;
		return 0;
	}
	return v;
}

/* push record -> fields */
static inline void
s8s_push_decode(struct s8s_push * p, uint32_t w0, uint32_t w1)
{
	p->w0 = w0;
	p->w1 = w1;
	p->type = w0 & 0xff;
	p->va = ((uint64_t)w1 << 8) | ((w0 >> 24) & 0xfc);   /* bit 1 of the low byte (0x22, 0xb2, 0x82 ...) is a flag, NOT an address bit: the data is 4-aligned (tests/comp_replay.c) */
	p->flags = (w0 >> 24) & 3;
	p->halves = (w0 >> 20) & 0x1f;
	if (p->halves == 0) {
		p->halves = 32;
	}
	p->dest = (w0 >> 8) & 0xff;
}

/* texture-state / attachment record (6 dwords) -> base address; returns the form (0 = not recognised) */
static inline int
s8s_texrec_va(const uint32_t * w, uint64_t * va)
{
	if (w[2] >= 0x10000000u && w[2] < 0x20000000u && (w[3] & 0xff) == 0 && (w[3] & 0x200) != 0) {
		*va = (uint64_t)w[2] << 4;
		return 1;
	}
	if ((w[2] & 0x40000000u) != 0) {
		*va = ((uint64_t)(w[2] & 0x3fffffffu) << 2) | S8S_HI;
		return 2;
	}
	return 0;
}

/* parse one vertex / fragment slot at `ptr` (low 32 bits) */
static inline void
s8s_parse_slot(s8s_rd_fn rd, void * ctx, uint32_t ptr, struct s8s_prog * pg)
{
	uint64_t base = S8S_HI | (ptr & ~0x1fu);
	uint32_t off = 0;
	int i;

	pg->found = 0;
	pg->slot = base;
	pg->slot_k = ptr & 0x1f;
	pg->npush = 0;
	pg->entry = pg->extra = pg->block = 0;
	pg->entry_flags = 0;
	pg->hdr = 0;
	for (i = 0; i < 48; i++) {
		int ok = 1;
		uint32_t w0 = s8s_u32(rd, ctx, base + off, &ok);
		uint32_t w1 = s8s_u32(rd, ctx, base + off + 4, &ok);

		if (!ok) {
			return;
		}
		if ((w0 & 0xf) == 0xf && ((w0 >> 16) == 0x0001 || (w0 >> 16) == 0x0011)) {
			uint32_t t;

			pg->hdr = w0;
			pg->entry = S8S_HI | (w1 & ~3u);
			pg->entry_flags = w1 & 3;
			off += 8;
			if (w0 & 0x400) {
				pg->extra = S8S_HI | s8s_u32(rd, ctx, base + off, &ok);
				off += 4;
			}
			t = s8s_u32(rd, ctx, base + off, &ok);
			if (t == 0x108) {
				off += 4;
				t = s8s_u32(rd, ctx, base + off, &ok);
			}
			if (ok && t == 0xc0400038u) {
				pg->block = S8S_HI | s8s_u32(rd, ctx, base + off + 4, &ok);
			}
			pg->found = 1;
			return;
		}
		if (((w0 & 0xf) == 3 || (w0 & 0xff) == 0x17) && (w1 >> 24) == 1) {
			if (pg->npush < S8S_MAXPUSH) {
				s8s_push_decode(&pg->push[pg->npush], w0, w1);
				pg->npush++;
			}
			off += 8;
			continue;
		}
		return;                         /* anything else ends the slot without a state record */
	}
}

struct s8s_state {
	struct s8s_draw d;              /* the persistent state; copied into every draw */
	uint32_t have_fs;
};

static inline void
s8s_zero(void * p, uint32_t n)
{
	uint8_t * b = (uint8_t *)p;
	uint32_t i;

	for (i = 0; i < n; i++) {
		b[i] = 0;
	}
}

static inline void
s8s_load(s8s_rd_fn rd, void * ctx, struct s8s_state * S, uint32_t n, uint32_t ptr, struct s8s_stats * st)
{
	uint32_t d[16];
	uint32_t i;
	int ok = 1;

	for (i = 0; i < n && i < 16; i++) {
		d[i] = s8s_u32(rd, ctx, (S8S_HI | ptr) + 4 * i, &ok);
	}
	if (!ok) {
		st->bad_ptr++;
		return;
	}
	st->loads++;
	if (n == 9 && d[0] == 0xc00) {
		S->d.vp_valid = 1;
		S->d.tile_x0 = (d[1] >> 16) & 0x7fff;
		S->d.tile_x1 = d[1] & 0xffff;
		S->d.tile_y0 = d[2] >> 16;
		S->d.tile_y1 = d[2] & 0xffff;
		for (i = 0; i < 4; i++) {
			S->d.vp[i] = d[3 + i];
		}
	} else if (n == 6 && d[0] == 0x1f) {
		for (i = 0; i < 6; i++) {
			S->d.pipe[i] = d[i];
		}
		S->d.blend = (d[2] >> 24) & 1;
	} else if (n == 5 && d[0] == 0xc0) {
		for (i = 0; i < 5; i++) {
			S->d.fs_bind[i] = d[i];
		}
		S->d.fs_tail = d[3] ? (S8S_HI | d[3]) : 0;
		S->d.fs_regs = d[4];
		s8s_parse_slot(rd, ctx, d[2], &S->d.fs);
		S->have_fs = 1;
	} else if (n == 3 && d[0] == 0x02000000u) {
		/* end-of-pass tile bounds: nothing to keep */
	} else if (n == 3 && d[0] == 0x00210000u) {
		/* constant 00210000 1e3ce508 000004a0 */
	} else if (n == 3) {
		for (i = 0; i < 3; i++) {
			S->d.sampler[i] = d[i];
		}
		S->d.sampler_va = S8S_HI | ptr;
	} else if (n == 2 && d[0] == 0x00020000u) {
		S->d.vsinfo[0] = d[0];
		S->d.vsinfo[1] = d[1];
	} else if (n == 2 && d[0] == 0x20) {
		S->d.drawslot[0] = d[0];
		S->d.drawslot[1] = d[1];
	} else {
		if (st->unknown_loads == 0) {
			st->first_unknown = ptr;
		}
		st->unknown_loads++;
	}
}

/*
 * Walk one pass stream from `stream` (the start pointer of the kernel command list block: a LINK) to its END, appending the draws.
 * `pass` is stored in each draw. Returns the number of draws appended (0..max). st may be NULL.
 */
static inline int
s8s_scan(s8s_rd_fn rd, void * ctx, uint64_t stream, uint32_t pass, struct s8s_draw * out, int max, struct s8s_stats * st)
{
	struct s8s_state S;
	struct s8s_stats local;
	uint64_t pc = stream;
	int nd = 0;
	int steps;

	if (st == 0) {
		st = &local;
	}
	s8s_zero(&S, sizeof(S));
	s8s_zero(st, sizeof(*st));
	for (steps = 0; steps < 20000 && nd < max; steps++) {
		int ok = 1;
		uint32_t w = s8s_u32(rd, ctx, pc, &ok);
		uint32_t h = w >> 24, m = (w >> 16) & 0xff;

		if (!ok) {
			st->bad_ptr++;
			break;
		}
		st->tokens++;
		if (w == 0x80000001u) {
			pc = S8S_HI | s8s_u32(rd, ctx, pc + 4, &ok);
			st->links++;
			continue;
		}
		if (w == 0xc0000000u) {
			st->end_seen = 1;
			break;
		}
		if ((w & 0xffff00ffu) == 1 && ((w >> 8) & 0xff) >= 1 && ((w >> 8) & 0xff) <= 16) {
			s8s_load(rd, ctx, &S, (w >> 8) & 0xff, s8s_u32(rd, ctx, pc + 4, &ok), st);
			pc += 8;
			continue;
		}
		if (h == 0x48 && m == 0x00 && (w & 0xffff) != 0) {
			S.d.launch0 = w;
			S.d.launch1 = s8s_u32(rd, ctx, pc + 4, &ok);
			S.d.vs_regs = s8s_u32(rd, ctx, pc + 12, &ok);
			s8s_parse_slot(rd, ctx, s8s_u32(rd, ctx, pc + 8, &ok), &S.d.vs);
			pc += 16;
			continue;
		}
		if (w == 0x20000160u) {
			pc += 4;                /* single-word token between a pass's skipped first chunk and its body (barrier / begin-body, HYPOTHESIS) */
			st->barriers++;
			continue;
		}
		if (h == 0x50) {
			S.d.index_mask = s8s_u32(rd, ctx, pc + 4, &ok);
			pc += 8;
			continue;
		}
		if ((h == 0x6e && m == 0x44) || (h == 0x7e && (m == 0x42 || m == 0x44))) {
			struct s8s_draw * d = &out[nd];
			uint32_t k = 0;

			*d = S.d;
			d->pass = pass;
			d->pc = pc;
			d->hdr = w;
			d->prim = (w >> 8) & 0xff;
			if (h == 0x6e) {
				d->kind = S8S_DRAW_LIST;
				d->count = s8s_u32(rd, ctx, pc + 4, &ok);
				d->instances = s8s_u32(rd, ctx, pc + 8, &ok) + 1;
				d->trailer = s8s_u32(rd, ctx, pc + 16, &ok);
				k = 20;
			} else if (m == 0x42) {
				d->kind = S8S_DRAW_INDEXED;
				d->index_va = S8S_HI | s8s_u32(rd, ctx, pc + 4, &ok);
				d->count = s8s_u32(rd, ctx, pc + 8, &ok);
				d->instances = s8s_u32(rd, ctx, pc + 12, &ok) + 1;
				d->trailer = s8s_u32(rd, ctx, pc + 20, &ok);
				k = 24;
			} else {
				d->kind = S8S_DRAW_OTHER;
				d->index_va = S8S_HI | s8s_u32(rd, ctx, pc + 4, &ok);
				d->trailer = s8s_u32(rd, ctx, pc + 20, &ok);
				k = 24;
			}
			nd++;
			st->draws++;
			pc += k;
			continue;
		}
		if (st->unknown_words == 0) {
			st->first_unknown = (uint32_t)pc;
		}
		st->unknown_words++;
		pc += 4;
	}
	return nd;
}

/*
 * Kernel command list (shmem type 1) -> passes. A pass block is recognised by {0x818, 0x7e8, 0x30, 1, <stream lo>, <stream hi = 1>} at block +0x28; the
 * block starts 0x28 before that (blocks are 0x820 bytes apart, the first one's header can be missing from a capture, so the marker is used).
 */
static inline int
s8s_find_passes(const uint32_t * cl, uint32_t ndw, struct s8s_pass * out, int max)
{
	int n = 0;
	uint32_t i;

	for (i = 0; i + 8 <= ndw && n < max; i++) {
		if (cl[i] == 0x818 && cl[i + 1] == 0x7e8 && cl[i + 2] == 0x30 && cl[i + 3] == 1 && cl[i + 5] == 1 && i >= 10) {
			uint32_t b = (i - 10) * 4;          /* i is the dword index of marker word 0 = block +0x28 */
			struct s8s_pass * p = &out[n];

			s8s_zero(p, sizeof(*p));
			p->block = b;
			p->stream = S8S_HI | cl[i + 4];
			if ((b + 0x698) / 4 < ndw) {
				p->width = cl[(b + 0x690) / 4];
				p->height = cl[(b + 0x694) / 4];
			}
			if ((b + 0x2d4) / 4 < ndw) {
				p->flags = cl[(b + 0x2a8) / 4];
				p->slotA = cl[(b + 0x2b0) / 4];
				p->slotB = cl[(b + 0x2d0) / 4];
			}
			n++;
			i += 8;
		}
	}
	return n;
}

/* render target of a pass: the first texture-state push (type 0x17) of its two slots, read as a 6-dword record */
static inline void
s8s_pass_target(s8s_rd_fn rd, void * ctx, struct s8s_pass * p)
{
	uint32_t s[2];
	int a, i, j;

	s[0] = p->slotB;
	s[1] = p->slotA;
	p->has_target = 0;
	for (a = 0; a < 2; a++) {
		struct s8s_prog pg;

		if (s[a] == 0) {
			continue;
		}
		s8s_parse_slot(rd, ctx, s[a], &pg);
		for (i = 0; i < (int)pg.npush; i++) {
			uint32_t w[6];
			int ok = 1;
			uint64_t va;
			int form;

			if (pg.push[i].type != S8S_PUSH_TEXTURE) {
				continue;
			}
			for (j = 0; j < 6; j++) {
				w[j] = s8s_u32(rd, ctx, pg.push[i].va + 4 * j, &ok);
			}
			form = ok ? s8s_texrec_va(w, &va) : 0;
			if (form != 0) {
				p->has_target = 1;
				p->target_va = va;
				p->target_fmt = w[0];
				p->target_w1 = w[1];
				p->target_form = form;
				return;
			}
		}
	}
}

#endif /* S8GFX_STREAM_H */
