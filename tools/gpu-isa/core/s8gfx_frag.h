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

#define S8F_REGS 64

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
	if (op == 0x12) {
		return 4;                       /* 12 25 00 00 (semantics unknown; G13 0x12 is icmpsel) */
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

		if (op == 0x51) {
			if (b[1] & 1) {                                 /* short iter (the texture path): the caller supplies the sample */
				pc += 4;
				continue;
			}
			int nch = (b[2] >> 6) ? (b[2] >> 6) : 4;
			int dreg = b[1] >> 2;

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

			if (b[4] & 0x20) {
				s1 = s8f_src(c, b, 3, 0);
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
