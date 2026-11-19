/*
 * Fragment program interpreter for the A11 (G10) shader subset decoded so far (tools/agx-emu.py is its Python twin, validated on the render scenes).
 * Integer-only (s8gfx_sf.h), no libc, so it builds into the kernel and into the host test (scratch/agx/frag_test.c).
 *
 *   iter 0x51   (8 B  51 <dst> <ch|..> .. ) channel count = byte2 >> 6 (0 = 4), dst byte1 = size32 bit1, reg (half units) = byte1 >> 2; values come from
 *               the caller-supplied varyings (the executor interpolates them per pixel)
 *   iter_short  (4 B  51 03 81 00) + texture_sample (10 B 21 ..): the texture path, handled by the caller (callback)
 *   fmul 0x4A / fadd 0x8A (6 B), ffma 0xCA (8 B)
 *   mov_imm 0x52 / 0xD2 (6 B)   wait_pix 0x48 (4 B)   wait 0x38 (2 B)   st_tile 0x09 / 0x89 (8 B)   end 08 01 00 00
 */
#ifndef S8GFX_FRAG_H
#define S8GFX_FRAG_H

#include "s8gfx_sf.h"
#include "s8gfx_tex.h"

#define S8F_REGS 64
#define S8F_NTEX 8

struct s8f_ctx {
	f32b r[S8F_REGS];               /* 32-bit registers, index = half-register index / 2 */
	f32b h[S8F_REGS * 2];           /* half registers (iter 0x10 form): kept as float bits (precision of a half is ignored) */
	f32b out[4];                    /* st_tile result */
	int stored;
	const f32b * varying;           /* the interpolated varyings of this pixel: varying[0..3] */
	int unknown;                    /* opcode byte of an instruction the interpreter does not know (0 = none) */
	int discard;
	/* vertex stage: the fetched vertex (up to 4 floats), and the exports by slot (0x40..0x4f -> exp[0..15]) */
	const f32b * fetch;
	f32b exp[16];
	uint32_t expmask;
	int nfetch;
	unsigned pc_end;                /* byte offset just past the end instruction of the last successful run */
	unsigned pc_fail;               /* byte offset of the instruction the interpreter did not know */
	/* vertex stage with the uniform file: uni[] is indexed by 16-bit half index (even indices hold the 32-bit words), pushed by the uniform-push records;
	 * rd reads a 32-bit word at a GPU virtual address (vertex attribute fetch); vid and stride address the vertex */
	uint32_t uni[128];
	uint32_t (*rd)(void * rdctx, uint64_t va);
	void * rdctx;
	uint32_t vid;
	uint32_t stride;
	/* fragment blending: the destination pixel as stored (BGRA8, little endian word) for ld_tile; has_tile = 0 leaves registers alone */
	uint32_t tile;
	int has_tile;
	/* integer / f16 fragment forms (see s8f_step_frag): scratch memory behind the device_load/device_store round trips (8-bit pack/unpack), the per-sample
	 * tile words for the raw four-way ld_tile (ntile_s = 0: every sample reads `tile`), the special registers read by get_sr (sr[48], sr[49] = pixel x, y in
	 * the tile), and the bookkeeping of forms whose semantics are only ASSUMED (assumed[op >> 5] bit op & 31; strict = stop instead of continuing) */
	uint8_t scratch[256];
	uint32_t tile_s[16];
	int ntile_s;
	uint32_t sr[256];
	uint32_t assumed[8];
	int strict;
	/* texture sampling (opcode 0x21, ASSUMED field mapping, see analysis/a11-isa-forms.md): the caller binds up to S8F_NTEX units (descriptor decoded with
	 * s8t_desc_decode / s8t_desc_make, sampler with s8t_sampler_decode) and a texel reader; ntex = 0 -> samples return transparent black (stored in out regs) and
	 * ntex_missing counts the samples. The unit index is (b5 & 63) % ntex until the push-record -> slot mapping is proven. */
	struct s8t_desc texd[S8F_NTEX];
	struct s8t_sampler texs[S8F_NTEX];
	int ntex;
	uint32_t (*trd)(void * ctx, uint64_t va);
	void * trdctx;
	unsigned ntex_missing;
	unsigned last_iter;             /* half index of the destination of the last iter: the coordinates a texture_sample reads (ASSUMED) */
	uint32_t hit[256];              /* executed instructions by opcode (coverage runner) */
};

/* 16-bit lane access to the 32-bit register file: half index i = register i / 2, low half first */
static inline uint16_t
s8f_gh(const struct s8f_ctx * c, unsigned i)
{
	uint32_t w = c->r[(i >> 1) & (S8F_REGS - 1)];

	return (i & 1) ? (uint16_t)(w >> 16) : (uint16_t)(w & 0xffff);
}

static inline void
s8f_sh(struct s8f_ctx * c, unsigned i, uint16_t v)
{
	uint32_t * w = &c->r[(i >> 1) & (S8F_REGS - 1)];

	if (i & 1) {
		*w = (*w & 0xffffu) | ((uint32_t)v << 16);
	} else {
		*w = (*w & 0xffff0000u) | v;
	}
}

/* ALU source byte (A11 compact forms): bits 6-7 = 0b10 / 0b11 -> register (6-bit half index; 32-bit operations use index / 2), else an immediate */
static inline uint32_t
s8f_s16(const struct s8f_ctx * c, uint8_t v)
{
	return (v >> 6) >= 2 ? s8f_gh(c, v & 0x3f) : v;
}

static inline uint32_t
s8f_s32(const struct s8f_ctx * c, uint8_t v)
{
	return (v >> 6) >= 2 ? c->r[((v & 0x3f) >> 1) & (S8F_REGS - 1)] : v;
}

static inline f32b
s8f_src(struct s8f_ctx * c, const uint8_t * b, int i, int src_is_half)
{
	uint8_t v = b[i];
	uint8_t idx = v & 0x3f;

	if (src_is_half) {
		return c->h[idx];
	}
	return c->r[idx / 2];
}

/*
 * Instruction length of the A11 forms seen in the corpus (0 = form not known). Lengths are PROVEN structurally: every captured vertex/fragment program
 * walks with these rules from its first instruction to its `08 01` end (tests/isa_walk.c). See analysis/a11-isa-forms.md.
 * Rules: 4 B = 4-byte mov family (low 6 bits 0x02/0x22: 02 42 82 a2 c2 e2), export 01/81, wait_pix 48/58, end 08, `23`, short iter 51 with bit 0;
 * 6/8 B = the 6-byte base form, 8 bytes when bit 47 (b5 bit 7) is set (iadd/device_load families); fixed: c6/ca/ld_tile/st_tile 8, mov_imm 6.
 * 0x1e (imadd) is 4 B (`1e 13 cc 00`, with a following 0x23), 8 B, or 6 B when b4 & 0x60 == 0x60, in which case a `20 c0 <imm32>` literal (6 B) follows.
 */
static inline unsigned
s8f_len(const uint8_t * b, unsigned avail)
{
	uint8_t op = b[0];
	unsigned lo = op & 0x3f;
	unsigned n = 0;

	if (avail < 2) {
		return 0;
	}
	if (op == 0x38) {
		return 2;
	}
	if (op == 0x08 || op == 0x48 || op == 0x58 || op == 0x01 || op == 0x81 || op == 0x23) {
		return 4;
	}
	if (lo == 0x02 || lo == 0x22) {
		return 4;
	}
	if (op == 0x51) {
		return (b[1] & 1) ? 4 : 8;
	}
	if (op == 0x52 || op == 0xd2) {
		return 6;
	}
	if (op == 0x21) {
		return 10;
	}
	if (op == 0x79 || op == 0x09 || op == 0x89 || op == 0xc6 || op == 0xca || op == 0x31) {
		return 8;
	}
	if (op == 0xf9 || op == 0xf6 || op == 0x76 || op == 0xfa || op == 0x7a) {
		return 8;                       /* f9 = ld_tile (second family), f6/76 = f16 three-source ALU, fa/7a = f16 ALU: 8 bytes (every program walk that contains them lands on the entry/end) */
	}
	if (op == 0x75) {
		return 2;                       /* `75 2a 75 29` before the end of the compositor's fragment programs (two 2-byte units; ASSUMED boundary) */
	}
	if (avail < 6) {
		return 0;
	}
	if (op == 0x1e && b[3] == 0x00 && (b[1] & 1) == 1 && b[2] >= 0xc0) {
		return 4;                       /* 1e 13 cc 00 / 1e 03 c8 00 */
	}
	if (op == 0x26) {
		return 6;                       /* 26 00 00 00 00 80: 6-byte form of the long compositor vertex programs (semantics unknown) */
	}
	if (op == 0x12 || op == 0x92) {
		return 4;                       /* 12 25 00 00 = 16-bit integer immediate move, 92 02 f8 5b = f16 immediate move (255.0) */
	}
	if (op == 0x16) {
		return 8;                       /* 16 90 e7 e7 00 e7 e7 00 (semantics unknown) */
	}
	if (op == 0x20) {
		return 6;                       /* `20 c0 <imm32>`: literal word that the preceding imadd (b4 & 0x60 == 0x60) takes as its operand */
	}
	if (op == 0x1e || op == 0x5e || op == 0x9e) {
		n = ((b[4] & 0x60) == 0x60) ? 6 : 8;
		return n;
	}
	if (op == 0x05 || op == 0x45 || op == 0x85 || op == 0xc5) {
		return (b[5] & 0x80) ? 8 : 6;
	}
	if (op == 0x0e || op == 0x4e || op == 0x8e || op == 0xce || op == 0x6e || op == 0x2e || op == 0xae || op == 0xee) {
		return (b[5] & 0x80) ? 8 : 6;
	}
	if (op == 0x4a || op == 0x8a || op == 0x0a || op == 0x86 || op == 0x46 || op == 0x7e || op == 0xfe || op == 0x3e || op == 0xbe || op == 0xde ||
	    op == 0x1a || op == 0x5a) {
		return 6;
	}
	return 0;
}

/* 16-bit float source byte: hint bits 0b10/0b11 -> half register (6-bit index), else the 8-bit minifloat immediate */
static inline f32b
s8f_h16src(const struct s8f_ctx * c, uint8_t v)
{
	return (v >> 6) >= 2 ? sf_h2f(s8f_gh(c, v & 0x3f)) : sf_minifloat(v);
}

/* float -> uint32 / int32, toward zero, saturating (NaN -> 0) */
static inline uint32_t
s8f_f2u(f32b a)
{
	uint32_t e = (a >> 23) & 0xff;
	uint64_t m;
	int x;

	if ((a >> 31) || e < 127) {
		return 0;
	}
	if (e == 0xff) {
		return (a & 0x7fffff) ? 0 : 0xffffffffu;
	}
	x = (int)e - 127;
	if (x >= 32) {
		return 0xffffffffu;
	}
	m = (a & 0x7fffff) | 0x800000;
	return (uint32_t)(x >= 23 ? (m << (x - 23)) : (m >> (23 - x)));
}

static inline int32_t
s8f_f2s(f32b a)
{
	uint32_t mag = s8f_f2u(a & 0x7fffffffu);

	if (sf_isnan(a)) {
		return 0;
	}
	if (a >> 31) {
		return mag >= 0x80000000u ? (int32_t)0x80000000u : -(int32_t)mag;
	}
	return mag > 0x7fffffffu ? 0x7fffffff : (int32_t)mag;
}

static inline void
s8f_assume(struct s8f_ctx * c, uint8_t op)
{
	c->assumed[op >> 5] |= 1u << (op & 31);
}

/* device_load / device_store with the G13 field layout (confirmed on the A11 by the g2/g3/g5/g7 round trips, analysis/a11-isa-forms.md):
 * F = bit7 | b1[1:0] << 1 | b6[0] << 3 (0 i8, 1 i16, 2 i32, 3 f16, 4 u8norm), mask = b6 >> 4, R = b1 >> 2 (+ b5[1:0] << 6) with b6 bit 1 = 32-bit registers,
 * offset O = b2 >> 4 | (b4 & 15) << 4 | b7 << 8 (immediate when b3 bit 0), shift = b5[3:2]; the byte address = O << (log2 element size + shift) in the scratch memory.
 * The base register pair is ignored (the compiler's own scratch slot). */
static inline unsigned
s8f_devldst(struct s8f_ctx * c, const uint8_t * b, unsigned avail)
{
	unsigned n = (b[5] & 0x80) ? 8 : 6;
	int store = (b[0] & 0x40) != 0;
	unsigned F = ((b[0] >> 7) & 1) | ((b[1] & 3) << 1) | ((b[6] & 1) << 3);
	unsigned mask = b[6] >> 4;
	unsigned R = (b[1] >> 2) | ((b[5] & 3) << 6);
	int r32 = (b[6] >> 1) & 1;
	uint32_t O = (b[2] >> 4) | ((b[4] & 15) << 4);
	unsigned sh = (b[5] >> 2) & 3;
	static const uint8_t esz[8] = { 0, 1, 2, 1, 0, 0, 1, 1 };
	unsigned esh = F < 8 ? esz[F] : 2;
	unsigned bytes = 1u << esh;
	unsigned addr, j = 0, k;

	if (avail < n) {
		return 0;
	}
	if (n == 8) {
		O |= (uint32_t)b[7] << 8;
	}
	if (b[3] & 1) {
		O = (uint32_t)(int32_t)(int16_t)O;
	} else {
		O = c->r[(O >> 1) & (S8F_REGS - 1)];
	}
	if (sh == 3) {
		sh = 2;
	}
	addr = (O << (esh + sh)) & 0xff;
	for (k = 0; k < 4; k++) {
		unsigned a = (addr + k * bytes) & 0xff;
		uint32_t v;

		if (!((mask >> k) & 1)) {
			continue;
		}
		if (store) {
			v = r32 ? c->r[((R >> 1) + j) & (S8F_REGS - 1)] : s8f_gh(c, R + j);
			if (F == 4) {
				c->scratch[a] = r32 ? sf_to_unorm8(v) : sf_to_unorm8(sf_h2f((uint16_t)v));
			} else if (F == 3) {
				v = r32 ? sf_f2h(v) : v;
				c->scratch[a] = (uint8_t)v;
				c->scratch[(a + 1) & 0xff] = (uint8_t)(v >> 8);
			} else {
				unsigned i;

				for (i = 0; i < bytes; i++) {
					c->scratch[(a + i) & 0xff] = (uint8_t)(v >> (8 * i));
				}
			}
		} else {
			if (F == 4) {
				f32b f = sf_div(sf_from_i64(c->scratch[a]), 0x437f0000u);

				v = r32 ? f : sf_f2h(f);
			} else {
				unsigned i;

				v = 0;
				for (i = 0; i < bytes; i++) {
					v |= (uint32_t)c->scratch[(a + i) & 0xff] << (8 * i);
				}
				if (F == 3 && r32) {
					v = sf_h2f((uint16_t)v);
				}
			}
			if (r32) {
				c->r[((R >> 1) + j) & (S8F_REGS - 1)] = v;
			} else {
				s8f_sh(c, R + j, (uint16_t)v);
			}
		}
		j++;
	}
	return n;
}

/*
 * Fragment-mode forms beyond the first decode (analysis/a11-isa-forms.md has the evidence and confidence of each). Returns the instruction length, or 0 when the
 * form is not handled here (the caller's older chain then decides). Operand size rule: b4 & 15 == 0 -> 16-bit lane op (dst half index = b1 >> 2),
 * otherwise 32-bit op (dst register = b1 >> 3).
 */
static inline unsigned
s8f_step_frag(struct s8f_ctx * c, const uint8_t * b, unsigned avail)
{
	uint8_t op = b[0];
	unsigned n = s8f_len(b, avail);
	int w32 = (b[4] & 15) != 0;

	if (n == 0 || n > avail) {
		return 0;
	}
	switch (op) {
	case 0x05: case 0x85: case 0x45: case 0xc5:
		return s8f_devldst(c, b, avail);
	case 0x79: case 0xf9:
		if (op == 0x79 && b[6] == 0xfa && b[7] == 0xff) {
			return 0;                       /* the float form of scene 9 */
		}
		{
			/* raw four-way ld_tile (compositor): dst half index b1 >> 2, four f16 unorm channels R G B A of the pixel word; sample index = b3[3:2] | b5[3:2] << 2
			 * ASSUMED: sample/format field split and channel order (see a11-isa-forms.md) */
			unsigned d = b[1] >> 2;
			unsigned smp = ((b[3] >> 2) & 3) | (((b[5] >> 2) & 3) << 2);
			uint32_t word = c->ntile_s ? c->tile_s[smp % (unsigned)c->ntile_s] : c->tile;
			unsigned k;

			for (k = 0; k < 4; k++) {
				static const uint8_t sh[4] = { 16, 8, 0, 24 };
				f32b f = sf_div(sf_from_i64((word >> sh[k]) & 0xff), 0x437f0000u);

				s8f_sh(c, d + k, sf_f2h(f));
			}
			s8f_assume(c, op);
			return n;
		}
	case 0x7e: {                                            /* bitop: r[b1>>3] = tt(A = b2, B = b3), tt = b4 >> 4 in G13 order (and 8, xor 6, or e) */
		uint32_t a, bb, res = 0;
		unsigned tt = b[4] >> 4;

		if (!w32) {                                     /* 16-bit lanes (b4 & 15 == 0): h[b1 >> 2] = tt(A, B) (compositor `7e 02 c0 81 80 00`; ASSUMED, same table) */
			a = s8f_s16(c, b[2]);
			bb = s8f_s16(c, b[3]);
			if ((b[3] >> 6) < 2) {
				bb |= (uint32_t)((b[5] >> 4) & 3) << 6;
			}
			if (tt & 1) res |= ~a & ~bb;
			if (tt & 2) res |= a & ~bb;
			if (tt & 4) res |= ~a & bb;
			if (tt & 8) res |= a & bb;
			s8f_sh(c, b[1] >> 2, (uint16_t)res);
			s8f_assume(c, op);
			return n;
		}
		a = s8f_s32(c, b[2]);
		bb = s8f_s32(c, b[3]);
		if ((b[3] >> 6) < 2) {
			bb |= (uint32_t)((b[5] >> 4) & 3) << 6;         /* 8-bit immediate: bits 7:6 live in b5 bits 5:4 (scene 31: and 0xf0 = b3 0x30 | 3 << 6) */
		}
		if (tt & 1) res |= ~a & ~bb;
		if (tt & 2) res |= a & ~bb;
		if (tt & 4) res |= ~a & bb;
		if (tt & 8) res |= a & bb;
		c->r[(b[1] >> 3) & (S8F_REGS - 1)] = res;
		return n;
	}
	case 0x0e: case 0x4e:                                   /* iadd (6 B) */
		if (n != 6) {
			return 0;
		}
		if (w32) {
			c->r[(b[1] >> 3) & (S8F_REGS - 1)] = s8f_s32(c, b[2]) + s8f_s32(c, b[3]);
		} else {
			s8f_sh(c, b[1] >> 2, (uint16_t)(s8f_s16(c, b[2]) + s8f_s16(c, b[3])));
		}
		return n;
	case 0x2e: case 0x6e: {                                 /* 0x6e bfeil: D = (a & ~mask) | ((b >> c) & mask); 0x2e bfi: D = (a & ~(mask << c)) | ((b & mask) << c);
								 * 8 B, A = b2, B = b3, C = b6, mask = (1 << m) - 1 with m = b5 & 0x3f (0 = all ones) */
		if (n != 8) {
			return 0;
		}
		if (w32) {
			uint32_t a = s8f_s32(c, b[2]), v = s8f_s32(c, b[3]), s = s8f_s32(c, b[6]) & 0x7f;
			unsigned m = b[5] & 0x3f;
			uint32_t mask = (m == 0 || m >= 32) ? 0xffffffffu : ((1u << m) - 1);
			uint32_t res;

			if (op == 0x6e) {
				res = (a & ~mask) | ((s >= 32 ? 0 : v >> s) & mask);
			} else {
				uint32_t sm = s >= 32 ? 0 : mask << s;

				res = (a & ~sm) | ((s >= 32 ? 0 : (v & mask) << s));
			}
			c->r[(b[1] >> 3) & (S8F_REGS - 1)] = res;
			return n;
		}
		if (op == 0x2e) {
			return 0;
		}
		{
			uint32_t v = s8f_s16(c, b[3]), s = s8f_s16(c, b[6]) & 0x7f;

			s8f_sh(c, b[1] >> 2, (uint16_t)(s >= 16 ? 0 : (v & 0xffff) >> s));
		}
		s8f_assume(c, op);                              /* the 16-bit form and the mask field m are not proven (the 32-bit shift with a = 0 is, by g3) */
		return n;
	}
	case 0x8e:                                              /* imadd 16-bit (8 B): D = A * B + C (C = b6) */
		if (n != 8 || w32) {
			return 0;
		}
		s8f_sh(c, b[1] >> 2, (uint16_t)(s8f_s16(c, b[2]) * s8f_s16(c, b[3]) + s8f_s16(c, b[6])));
		s8f_assume(c, op);
		return n;
	case 0xc6: {                                            /* f16 ffma (8 B): D = a * b + c; b: register (b3 bits 7:6 >= 2) else minifloat(b3); c: register when b5 bits 7:6 >= 2 else minifloat(b5) */
		f32b a = sf_h2f(s8f_gh(c, b[2] & 0x3f));
		f32b m = s8f_h16src(c, b[3]);
		f32b cc = s8f_h16src(c, b[5]);

		if (b[4] & 0x02) {
			a = sf_neg(a);                          /* ASSUMED: negate-A flag in b4 bit 1, as in the 32-bit forms */
		}
		s8f_sh(c, b[1] >> 2, sf_f2h(sf_fma(a, m, cc)));
		s8f_assume(c, op);
		return n;
	}
	case 0x46: case 0x86: {                                 /* f16 fmul (46) / fadd (86), 6 B: D = a op b; b3: register or minifloat; b4 bit 1 = negate a (`86 1a c6 30 02 00` = 1.0 - h6) */
		f32b a = sf_h2f(s8f_gh(c, b[2] & 0x3f));
		f32b bb = s8f_h16src(c, b[3]);

		if (b[4] & 0x02) {
			a = sf_neg(a);
		}
		s8f_sh(c, b[1] >> 2, sf_f2h(op == 0x46 ? sf_mul(a, bb) : sf_add(a, bb)));
		s8f_assume(c, op);                              /* family guess: 0x06 + bits 7:6 = fmul/fadd/ffma at 16 bit, like 0x0a for 32 bit (46 = fmul is only inferred) */
		return n;
	}
	case 0xf6: case 0x76: {                                 /* 8 B with the operand pair repeated in b5/b6: ASSUMED f16 multiply D = a * b (b2, b3), second pair ignored */
		f32b a = sf_h2f(s8f_gh(c, b[2] & 0x3f));
		f32b bb = sf_h2f(s8f_gh(c, b[3] & 0x3f));

		s8f_sh(c, b[1] >> 2, sf_f2h(sf_mul(a, bb)));
		s8f_assume(c, op);
		return n;
	}
	case 0xfa: case 0x7a: case 0x1a: case 0x5a: {           /* 32-bit float multiply with the operand pair repeated (fa/7a 8 B, 1a/5a 6 B): ASSUMED D = a * b; dst register (b1 - 1) / 8 (bit 0 = 1) or b1 >> 3 */
		f32b a = c->r[((b[2] & 0x3f) >> 1) & (S8F_REGS - 1)];
		f32b bb = c->r[((b[3] & 0x3f) >> 1) & (S8F_REGS - 1)];

		c->r[(b[1] >> 3) & (S8F_REGS - 1)] = sf_mul(a, bb);
		s8f_assume(c, op);
		return n;
	}
	case 0x21: {                                            /* texture_sample (10 B), ASSUMED mapping (G13 numbering does not apply): dst register b1 >> 2 (32-bit), channel mask b4 & 15 (contiguous destination),
								 * texture unit (b5 & 63) % ntex, sampler = same unit, coordinates = the registers the last iter wrote (u, v as floats) */
		f32b out4[4];
		unsigned mask = b[4] & 15, d = b[1] >> 2, k, j = 0;
		unsigned u = c->ntex ? (unsigned)(b[5] & 63) % (unsigned)c->ntex : 0;
		f32b uu = c->r[(c->last_iter / 2) & (S8F_REGS - 1)], vv = c->r[(c->last_iter / 2 + 1) & (S8F_REGS - 1)];

		if (c->ntex > 0 && c->trd != 0) {
			s8t_sample(&c->texd[u], &c->texs[u], uu, vv, c->trd, c->trdctx, out4);
		} else {
			c->ntex_missing++;
			for (k = 0; k < 4; k++) {
				out4[k] = SF_ZERO;
			}
		}
		for (k = 0; k < 4; k++) {
			if ((mask >> k) & 1) {
				c->r[(d + j) & (S8F_REGS - 1)] = out4[k];
				j++;
			}
		}
		s8f_assume(c, op);
		return n;
	}
	case 0x82: case 0xa2:                                   /* get_sr, 16-bit: h[b1] = sr[b2] */
		if (op == 0xa2 || op == 0x82) {
			s8f_sh(c, b[1], (uint16_t)c->sr[b[2]]);
			s8f_assume(c, op);
			return n;
		}
		return 0;
	case 0x58:                                              /* signal_pix (4 B), the pixel-ordering partner of wait_pix: no effect on a single pixel */
		return n;
	case 0xe2: case 0x22:                                   /* get_sr, 32-bit form (`e2 06 10 04`): r[b1 >> 1] = sr[b2] (ASSUMED; 0x22 only length-known) */
		c->r[(b[1] >> 1) & (S8F_REGS - 1)] = c->sr[b[2]];
		s8f_assume(c, op);
		return n;
	case 0x02:                                              /* 02 04 04 00: 16-bit register copy (a self copy in the corpus) */
		s8f_sh(c, b[1], s8f_gh(c, b[2]));
		s8f_assume(c, op);
		return n;
	case 0x31:                                              /* end-of-shader pixel write: the compositor programs have no st_tile; r(h0..h3) = f16 RGBA is the colour */
		c->out[0] = sf_h2f(s8f_gh(c, 0));
		c->out[1] = sf_h2f(s8f_gh(c, 1));
		c->out[2] = sf_h2f(s8f_gh(c, 2));
		c->out[3] = sf_h2f(s8f_gh(c, 3));
		c->stored = 1;
		s8f_assume(c, op);
		return n;
	case 0xbe: {                                            /* convert (6 B): r[b1>>3] = conv(src b3), mode = b2 (G13 numbering: 8 f_to_u32, 9 f_to_s32, 10 u32_to_f, 11 s32_to_f,
								 * 0 u8_to_f, 6 u16_to_f), b4 bit 6 = round to nearest (else toward zero for float -> int) */
		uint32_t v = s8f_s32(c, b[3]);
		uint32_t res;

		switch (b[2] & 0x3f) {
		case 8: res = s8f_f2u(v); break;
		case 9: res = (uint32_t)s8f_f2s(v); break;
		case 10: res = sf_from_i64((int64_t)v); break;
		case 11: res = sf_from_i64((int64_t)(int32_t)v); break;
		case 0: res = sf_from_i64(v & 0xff); break;
		case 6: res = sf_from_i64(v & 0xffff); break;
		default: return 0;
		}
		if ((b[2] & 0x3f) != 8 && (b[2] & 0x3f) != 9 && (b[2] & 0x3f) != 10 && (b[2] & 0x3f) != 11) {
			s8f_assume(c, op);                      /* u8/u16 sources: mode numbers from G13, not seen on the A11 */
		}
		c->r[(b[1] >> 3) & (S8F_REGS - 1)] = res;
		return n;
	}
	case 0x92:                                              /* 4 B: h[b1] = 16-bit immediate b2 | b3 << 8 (scene 31/35: 255.0 = 0x5bf8 in h2, read by `4a 03 c0 c2 04 00` as a 16-bit source;
								 * compositor: `92 01 00 7c` h1 = +inf, `92 03 00 3c` h3 = 1.0) */
		s8f_sh(c, b[1], (uint16_t)(b[2] | (b[3] << 8)));
		return n;
	case 0x12:                                              /* 4 B: h[b1] = 16-bit integer immediate (ASSUMED) */
		s8f_sh(c, b[1], (uint16_t)(b[2] | (b[3] << 8)));
		s8f_assume(c, op);
		return n;
	case 0xc2: case 0x42:                                   /* 32-bit uniform copy into the half-register pair at half index b1 */
		c->r[(b[1] >> 1) & (S8F_REGS - 1)] = c->uni[b[2] & 127];
		return n;
	case 0x75: case 0xde: case 0xfe: case 0x1e: case 0x20: case 0x23: case 0x26:
		s8f_assume(c, op);                              /* decoded for length only: treated as no-ops */
		return n;
	default:
		return 0;
	}
}

/* run a fragment program of at most len bytes; returns 1 when a st_tile ran (out[] valid) */
static inline int
s8f_run(struct s8f_ctx * c, const uint8_t * p, unsigned len)
{
	unsigned pc = 0;

	c->stored = 0;
	c->unknown = 0;
	c->discard = 0;
	while (pc + 2 <= len) {
		const uint8_t * b = p + pc;
		uint8_t op = b[0];

		c->hit[op]++;
		if (c->rd == 0 && c->fetch == 0) {
			uint32_t was = 0, now = 0;
			unsigned adv, i;

			for (i = 0; i < 8; i++) {
				was ^= c->assumed[i] * (2 * i + 1);
			}
			adv = s8f_step_frag(c, b, len - pc);
			if (adv) {
				for (i = 0; i < 8; i++) {
					now ^= c->assumed[i] * (2 * i + 1);
				}
				if (c->strict && now != was) {
					c->unknown = op;
					c->pc_fail = pc;
					return 0;
				}
				pc += adv;
				continue;
			}
		}
		if (op == 0x51) {
			int nch = (b[2] >> 6) ? (b[2] >> 6) : 4;
			int dreg = b[1] >> 2;

			c->last_iter = (unsigned)dreg;
			if (b[1] & 1) {                                 /* short iter (4 B, the texture coordinates): same fields as the long form, values from the caller's varyings (ASSUMED equal semantics) */
				for (int k = 0; k < nch; k++) {
					c->r[(dreg / 2 + k) & (S8F_REGS - 1)] = c->varying ? c->varying[k] : 0;
				}
				pc += 4;
				continue;
			}

			for (int k = 0; k < nch; k++) {
				if (b[1] & 2) {
					c->r[(dreg / 2 + k) & (S8F_REGS - 1)] = c->varying ? c->varying[k] : 0;
				} else {
					c->h[(dreg + k) & (S8F_REGS * 2 - 1)] = c->varying ? c->varying[k] : 0;
				}
			}
			pc += 8;
		} else if (op == 0x4A || op == 0x8A) {                  /* fmul / fadd */
			int d = (b[1] - 1) / 8;
			int half_a = !(b[4] & 0x04) && (b[2] & 0x3f) >= 4;
			f32b a = s8f_src(c, b, 2, half_a);
			f32b s1;

			if (b[4] & 0x20) {                              /* b4 bit 5: B is a 32-bit register */
				s1 = s8f_src(c, b, 3, 0);
			} else if ((b[3] >> 6) >= 2) {                  /* hint bits 0b10/0b11 in b3 without bit 5: a 16-bit register, converted (scene 35: `4a 03 c0 c2 04 00` = r0 * f32(h2 = 255.0)) */
				s1 = sf_h2f(s8f_gh(c, b[3] & 0x3f));
			} else {
				/* 8-bit minifloat: bits 0-5 in byte 3, bits 6-7 in bits 4-5 of byte 5 (seen with 2.0 = 0x40 in the vertex col*2 shader) */
				s1 = sf_minifloat((uint8_t)(b[3] | (((b[5] >> 4) & 3) << 6)));
			}
			if (b[4] & 0x02) {
				a = sf_neg(a);
			}
			c->r[d & (S8F_REGS - 1)] = (op == 0x4A) ? sf_mul(a, s1) : sf_add(a, s1);
			pc += 6;
		} else if (op == 0xCA) {                                /* ffma, saturate in byte 7 bit 4, negate src2 bit 7 */
			int d = (b[1] - 1) / 8;
			f32b a = s8f_src(c, b, 2, 0);
			f32b s1, s2, v;

			if (b[4] & 0x20) {
				s1 = s8f_src(c, b, 3, 0);
			} else {
				s1 = sf_minifloat((uint8_t)(b[3] | ((b[4] >> 6) << 6)));
			}
			if (b[6] & 0x04) {
				s2 = c->r[((b[5] & 0x3f) / 2) & (S8F_REGS - 1)];        /* third operand is a register (matrix rows, bias from a uniform) */
			} else {
				s2 = sf_minifloat(b[5]);
			}
			if (b[7] & 0x80) {
				s2 = sf_neg(s2);
			}
			v = sf_fma(a, s1, s2);
			if (b[7] & 0x10) {
				v = sf_sat(v);
			}
			c->r[d & (S8F_REGS - 1)] = v;
			pc += 8;
		} else if (op == 0x52 || op == 0xD2) {                  /* mov_imm */
			c->r[(b[1] / 2) & (S8F_REGS - 1)] = (uint32_t)b[2] | ((uint32_t)b[3] << 8) | ((uint32_t)b[4] << 16) | ((uint32_t)b[5] << 24);
			pc += 6;
		} else if (op == 0x48) {
			pc += 4;
		} else if (op == 0x38) {
			pc += 2;
		} else if (op == 0x09 || op == 0x89) {                  /* st_tile: r0..r3 */
			c->out[0] = c->r[0];
			c->out[1] = c->r[1];
			c->out[2] = c->r[2];
			c->out[3] = c->r[3];
			c->stored = 1;
			pc += 8;
		} else if (op == 0x45 && pc + 8 <= len) {
			/* pack_unorm4x8 (8 B, fragment stage; seen in the pack/unpack round-trip scene): r[b1/2] = unorm8(r0) | unorm8(r1) << 8 | unorm8(r2) << 16 |
			 * unorm8(r3) << 24; the sources are read before the destination is written */
			uint32_t v = (uint32_t)sf_to_unorm8(c->r[0]) | ((uint32_t)sf_to_unorm8(c->r[1]) << 8) | ((uint32_t)sf_to_unorm8(c->r[2]) << 16) |
			    ((uint32_t)sf_to_unorm8(c->r[3]) << 24);

			c->r[(b[1] / 2) & (S8F_REGS - 1)] = v;
			pc += 8;
		} else if (op == 0x05 && c->rd == 0 && pc + 8 <= len) {
			/* unpack_unorm4x8 (8 B): r0..r3 = byte k of r[b1/2] / 255 */
			uint32_t v = c->r[(b[1] / 2) & (S8F_REGS - 1)];
			f32b k255 = 0x437f0000u;

			for (int k = 0; k < 4; k++) {
				c->r[k] = sf_div(sf_from_i64((v >> (8 * k)) & 0xff), k255);
			}
			pc += 8;
		} else if (op == 0x79 && pc + 8 <= len && c->has_tile) {
			/* ld_tile (8 B): 79 02 00 00 00 00 fa ff loads the destination pixel into r0..r3 = R, G, B, A as floats (BGRA8 target); the compositor's blend
			 * shaders use four of these with other flag bytes, which are not decoded yet (they leave the registers unchanged here) */
			if (b[6] == 0xfa && b[7] == 0xff) {
				f32b k = 0x437f0000u;                          /* 255.0f */

				c->r[0] = sf_div(sf_from_i64((c->tile >> 16) & 0xff), k);
				c->r[1] = sf_div(sf_from_i64((c->tile >> 8) & 0xff), k);
				c->r[2] = sf_div(sf_from_i64(c->tile & 0xff), k);
				c->r[3] = sf_div(sf_from_i64((c->tile >> 24) & 0xff), k);
			}
			pc += 8;
		} else if (op == 0x01 || op == 0x81) {                  /* export (vertex stage): 01 <8*reg+1> <slot> 80 */
			unsigned slot = b[2];
			unsigned reg = (b[1] - 1) / 8;

			if (slot >= 0x40 && slot < 0x50) {
				c->exp[slot - 0x40] = c->r[reg & (S8F_REGS - 1)];
				c->expmask |= 1u << (slot - 0x40);
			}
			pc += 4;
		} else if (c->rd != 0 && (op == 0xc2 || op == 0x42) && pc + 4 <= len && b[3] == 0x00) {
			/* c2 dst src 00: copy the 32-bit uniform at half index src into the register at half index dst */
			c->r[(b[1] / 2) & (S8F_REGS - 1)] = c->uni[b[2] & 127];
			pc += 4;
		} else if (c->rd != 0 && op == 0x0e && pc + 6 <= len) {
			c->stride = (uint32_t)b[5] * 4;                 /* 0e 00 c0 ca 1a 04: the vertex address multiply-add, stride 16 in these programs */
			pc += 6;
		} else if (c->rd != 0 && op == 0x8e && pc + 8 <= len) {
			c->stride = b[3];                               /* 8e 00 ca <stride> 26 80 c0 00 (stride 32 = 0x20 in the interleaved probe) */
			pc += 8;
		} else if (c->rd != 0 && (op == 0x05 || op == 0x85) && pc + 8 <= len) {
			/* device_load (8 B): 05 <8*dst+1> <byte offset> 05 00 <c4|c8> <NN> 00; components = 2 + bit6 + bit7 of NN; address = pointer in uniform
			 * halves 0..3 + vid * stride + offset */
			int n = 2 + ((b[6] >> 6) & 1) + ((b[6] >> 7) & 1);
			unsigned dst = (b[1] - 1) / 8;
			uint64_t base = (uint64_t)c->uni[0] | ((uint64_t)c->uni[2] << 32);
			uint64_t va = base + (uint64_t)c->vid * c->stride + b[2];

			for (int k = 0; k < n; k++) {
				c->r[(dst + k) & (S8F_REGS - 1)] = c->rd(c->rdctx, va + 4 * (uint64_t)k);
			}
			pc += 8;
		} else if (c->rd == 0 && op == 0xc2 && pc + 20 <= len && b[1] == 0x02 && b[4] == 0x0e && b[10] == 0x05 && b[18] == 0x38) {
			/* vertex fetch prologue (20 B): c2 02 02 00 | 0e 00 c0 ca 1a 04 | 05 01 00 05 00 cX NN 00 | 38 00; components loaded = 2 + bit6 + bit7 of NN */
			int n = 2 + ((b[16] >> 6) & 1) + ((b[16] >> 7) & 1);

			for (int k = 0; k < n && k < c->nfetch; k++) {
				c->r[k] = c->fetch[k];
			}
			pc += 20;
		} else if (op == 0xc2 && pc + 4 <= len && b[3] == 0x00) {
			pc += 4;                                        /* c2 xx yy 00: filler / register-copy form seen around the prologue; treated as a no-op for now */
		} else if ((op == 0x1e || op == 0x20 || op == 0x23 || op == 0x26 || op == 0x12 || op == 0x16) && s8f_len(b, len - pc) != 0 && s8f_len(b, len - pc) <= len - pc) {
			/* imadd with a literal (`1e .. 60 00` + `20 c0 <imm32>`): the vertex address multiply, stride = the literal (ASSUMED: the literal is the byte stride of
			 * the attribute stream); the 4-byte 1e/23 pair and the 6-byte `20` literal word are skipped (ASSUMED no-ops for the position math) */
			unsigned n = s8f_len(b, len - pc);

			if (op == 0x1e && n == 6 && pc + 12 <= len && b[6] == 0x20 && b[7] == 0xc0 && c->rd != 0) {
				c->stride = (uint32_t)b[8] | ((uint32_t)b[9] << 8) | ((uint32_t)b[10] << 16) | ((uint32_t)b[11] << 24);
			}
			s8f_assume(c, op);
			pc += n;
		} else if (op == 0x08 && pc + 4 <= len && b[1] == 0x01) {
			c->pc_end = pc + 4;
			return c->stored || c->expmask != 0;
		} else {
			c->unknown = op;
			c->pc_fail = pc;
			return 0;
		}
	}
	return c->stored;
}

/*
 * Fragment program image start (tests/frag_forms.c: analysis/compositor-frag-forms.md section 0.3, compositor-stream.md).
 * m/len = memory window of the image, block = offset of the program block the draw's `c0400038 <block VA>` record names ((unsigned)-1 if the draw has none),
 * entry = offset of the entry pointer (flag bits cleared; the pointer's `& 3 == 2` flag shifts the instruction boundary to entry + 2 for some programs).
 * Order: (1) block + 0x40 when the instruction-length walk from there lands on the entry (or entry + 2) and reaches `08 01` after it;
 *        (2) the earliest offset in [entry - 0x100, entry] whose walk does the same (no block, or an image with another header);
 *        (3) block + 0x40 unverified when the block exists and its first byte decodes (the walk stops on a form not known yet).
 * Returns 1, 2 or 3 accordingly with *start set, 0 when there is no candidate.
 */
static inline int
s8f_walk_lands(const uint8_t * m, unsigned len, unsigned start, unsigned entry)
{
	unsigned pc = start;
	int hit = start == entry || start == entry + 2;

	while (pc + 2 <= len) {
		unsigned n = s8f_len(m + pc, len - pc);

		if (n == 0 || pc + n > len) {
			return 0;
		}
		if (m[pc] == 0x08 && m[pc + 1] == 0x01) {
			if (hit) {
				return 1;
			}
			if (pc + 4 > entry) {
				return 0;
			}
			pc += 4;                        /* an earlier program of the same image: skip its end and the 08 00 padding */
			while (pc + 2 <= len && m[pc] == 0x08 && m[pc + 1] == 0x00) {
				pc += 2;
			}
			if (pc == entry || pc == entry + 2) {
				hit = 1;
			}
			continue;
		}
		pc += n;
		if (pc == entry || pc == entry + 2) {
			hit = 1;
		}
	}
	return 0;
}

static inline int
s8f_start_image(const uint8_t * m, unsigned len, unsigned block, unsigned entry, unsigned * start)
{
	unsigned q;

	if (block != (unsigned)-1 && block + 0x40 < len && s8f_walk_lands(m, len, block + 0x40, entry)) {
		*start = block + 0x40;
		return 1;
	}
	for (q = entry >= 0x100 ? entry - 0x100 : 0; q <= entry; q++) {
		if (s8f_walk_lands(m, len, q, entry)) {
			*start = q;
			return 2;
		}
	}
	if (block != (unsigned)-1 && block + 0x40 < len && s8f_len(m + block + 0x40, len - block - 0x40) != 0) {
		*start = block + 0x40;
		return 3;
	}
	return 0;
}

/*
 * Find the real start of a vertex program from the entry pointer of its state record: the pointer can land in the middle of a constant-setup
 * instruction. Anchor on the fetch prologue (c2 02 02 00 0e 00 c0 ca 1a 04 05 01) within [off-8, off+0x40], then take the optional filler
 * (c2 00 00 00) and the contiguous run of mov_imm (52 .. 6 bytes) before it. Without a prologue the entry is used as is.
 * m = memory of the region, len = its length, off = pointer offset in it; returns the start offset.
 */
static inline unsigned
s8v_find_start(const uint8_t * m, unsigned len, unsigned off)
{
	static const uint8_t sig[5] = { 0xc2, 0x02, 0x02, 0x00, 0x0e };    /* 0x0e or 0x8e (the address multiply-add) follows */
	unsigned lo = off >= 0x30 ? off - 0x30 : 0;      /* the entry pointer lands inside the prologue or up to ~0x20 bytes into the constant run */
	unsigned q;

	for (q = lo; q <= off + 0x40 && q + 12 <= len; q++) {
		unsigned k;

		for (k = 0; k < 4 && m[q + k] == sig[k]; k++) {
		}
		if (k == 4 && (m[q + 4] == 0x0e || m[q + 4] == 0x8e)) {
			unsigned s = q;
			int n = 0;

			if (s >= 4 && m[s - 4] == 0xc2 && m[s - 3] == 0 && m[s - 2] == 0 && m[s - 1] == 0) {
				s -= 4;
			}
			while (s >= 6 && m[s - 6] == 0x52 && n < 16) {
				s -= 6;
				n++;
			}
			return s;
		}
	}
	/* no fetch prologue: the earliest start in [off-0x20, off] whose run completes with a position export and reaches past the entry pointer
	 * (an earlier shader's own end instruction stops a run before the pointer, so it cannot be mistaken for this one) */
	for (q = off >= 0x20 ? off - 0x20 : 0; q <= off; q++) {
		struct s8f_ctx c;
		f32b zero[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
		unsigned i;

		for (i = 0; i < sizeof(c); i++) {
			((uint8_t *)&c)[i] = 0;
		}
		c.fetch = zero;
		c.nfetch = 4;
		c.varying = zero;               /* a candidate that starts on an interpolation instruction must not read a null array */
		if (s8f_run(&c, m + q, len - q) && (c.expmask & 0xf) == 0xf && q + c.pc_end > off) {
			return q;
		}
	}
	return off;
}

#endif /* S8GFX_FRAG_H */
