/*
 * Integer-only IEEE-754 single precision (round to nearest even) for the kernel-side AGX executor: the XNU kernel is built without floating point.
 * Floats are bit patterns (uint32_t). Denormal inputs are treated as zero and denormal results flush to zero (what the GPU does, too).
 * sf_mul, sf_add, sf_sub, sf_fma (single rounding), sf_neg, sf_abs, sf_sat, sf_to_unorm8; tested bit-exact against the host FPU: tools in scratch/agx/sf_test.c.
 */
#ifndef S8GFX_SF_H
#define S8GFX_SF_H

#include <stdint.h>

typedef uint32_t f32b;

#define SF_ONE   0x3f800000u
#define SF_ZERO  0x00000000u
#define SF_QNAN  0x7fc00000u
#define SF_INF   0x7f800000u

static inline int sf_isnan(f32b a) { return ((a >> 23) & 0xff) == 0xff && (a & 0x7fffff); }
static inline int sf_isinf(f32b a) { return ((a >> 23) & 0xff) == 0xff && !(a & 0x7fffff); }
static inline f32b sf_neg(f32b a) { return a ^ 0x80000000u; }
static inline f32b sf_abs(f32b a) { return a & 0x7fffffffu; }

/* unpack: returns 0 for a zero or denormal (value flushed), exponent field in *e (1..254), 24-bit mantissa with the implicit bit in *m */
static inline int
sf_unpack(f32b a, uint32_t * s, int32_t * e, uint32_t * m)
{
	*s = a >> 31;
	*e = (int32_t)((a >> 23) & 0xff);
	if (*e == 0) {
		*m = 0;
		return 0;
	}
	*m = (a & 0x7fffff) | 0x800000;
	return 1;
}

/* round-to-nearest-even pack of sign * mant * 2^(exp - 150): mant is any 64-bit value (sticky already folded into bit 0 by the caller if needed) */
static inline f32b
sf_pack64(uint32_t s, int32_t exp, unsigned __int128 mant)
{
	int msb;
	int shift;
	uint64_t keep;
	uint64_t rest;
	uint64_t half;

	if (mant == 0) {
		return s << 31;
	}
	msb = 127;
	while (!((mant >> msb) & 1)) {
		msb--;
	}
	/* value = mant * 2^(exp-150); we want 24 significant bits: shift right by (msb - 23) */
	shift = msb - 23;
	exp += shift;       /* exponent field of the result before rounding: value = (mant >> shift) * 2^(exp-150) */
	if (shift > 0) {
		unsigned __int128 mask = (((unsigned __int128)1) << shift) - 1;
		rest = (uint64_t)0;
		keep = (uint64_t)(mant >> shift);
		unsigned __int128 low = mant & mask;
		unsigned __int128 hf = ((unsigned __int128)1) << (shift - 1);
		if (low > hf || (low == hf && (keep & 1))) {
			keep++;
			if (keep == 0x1000000) {
				keep >>= 1;
				exp++;
			}
		}
		(void)rest;
		(void)half;
	} else {
		keep = (uint64_t)(mant << (-shift));
	}
	/* now value = keep * 2^(exp-150), keep in [2^23, 2^24): exponent field = exp */
	if (exp >= 255) {
		return (s << 31) | SF_INF;
	}
	if (exp <= 0) {
		return s << 31;     /* flush denormals */
	}
	return (s << 31) | ((uint32_t)exp << 23) | ((uint32_t)keep & 0x7fffff);
}

static inline f32b
sf_mul(f32b a, f32b b)
{
	uint32_t sa, sb, ma, mb;
	int32_t ea, eb;
	int za = !sf_unpack(a, &sa, &ea, &ma), zb = !sf_unpack(b, &sb, &eb, &mb);

	if (sf_isnan(a) || sf_isnan(b)) {
		return SF_QNAN;
	}
	if (sf_isinf(a) || sf_isinf(b)) {
		if (za || zb) {
			return SF_QNAN;
		}
		return ((sa ^ sb) << 31) | SF_INF;
	}
	if (za || zb) {
		return (sa ^ sb) << 31;
	}
	/* (ma * 2^(ea-150)) * (mb * 2^(eb-150)) = (ma*mb) * 2^(ea+eb-300) = P * 2^((ea+eb-150) - 150) */
	return sf_pack64(sa ^ sb, ea + eb - 150, (unsigned __int128)((uint64_t)ma * (uint64_t)mb));
}

/* a + b where both are finite; magnitudes aligned with 128-bit arithmetic and a sticky bit */
static inline f32b
sf_add_core(uint32_t sa, int32_t ea, unsigned __int128 ma, uint32_t sb, int32_t eb, unsigned __int128 mb)
{
	/* value a = ma * 2^(ea-150), b = mb * 2^(eb-150); bring both to the smaller exponent with headroom */
	int32_t e;
	int d;
	unsigned __int128 big, small;
	uint32_t sbig, ssmall;
	int32_t ebig;
	unsigned __int128 sum;
	int sticky = 0;

	if (ma == 0) {
		return sf_pack64(sb, eb, mb);
	}
	if (mb == 0) {
		return sf_pack64(sa, ea, ma);
	}
	if (ea >= eb) {
		big = ma; small = mb; sbig = sa; ssmall = sb; ebig = ea; d = ea - eb;
	} else {
		big = mb; small = ma; sbig = sb; ssmall = sa; ebig = eb; d = eb - ea;
	}
	/* big has at most 49 bits (fma product) so shifting it left by 64 keeps it below 2^113 */
	big <<= 64;
	e = ebig - 64;
	if (d >= 120) {
		small = 0;
		sticky = 1;
	} else {
		small <<= 64;
		if (d > 0) {
			unsigned __int128 lost = small & ((((unsigned __int128)1) << d) - 1);
			small >>= d;
			if (lost) {
				sticky = 1;
			}
		}
	}
	if (sbig == ssmall) {
		sum = big + small;
	} else if (big >= small) {
		sum = big - small;
		if (sticky) {
			sum--;           /* the true subtrahend is slightly larger than the truncated one */
		}
	} else {
		sum = small - big;
	}
	if (sum == 0 && !sticky) {
		return 0;
	}
	if (sticky) {
		sum = (sum << 1) | 1;   /* keep the sticky information below the rounding bit */
		e -= 1;
	}
	return sf_pack64(big >= small || sbig == ssmall ? sbig : ssmall, e, sum);
}

static inline f32b
sf_add(f32b a, f32b b)
{
	uint32_t sa, sb, ma, mb;
	int32_t ea, eb;
	int za = !sf_unpack(a, &sa, &ea, &ma), zb = !sf_unpack(b, &sb, &eb, &mb);

	if (sf_isnan(a) || sf_isnan(b)) {
		return SF_QNAN;
	}
	if (sf_isinf(a)) {
		return (sf_isinf(b) && sa != sb) ? SF_QNAN : a;
	}
	if (sf_isinf(b)) {
		return b;
	}
	if (za && zb) {
		return (sa & sb) << 31;
	}
	if (za) {
		return b;
	}
	if (zb) {
		return a;
	}
	return sf_add_core(sa, ea, ma, sb, eb, mb);
}

static inline f32b sf_sub(f32b a, f32b b) { return sf_add(a, sf_neg(b)); }

/* fused multiply-add: a * b + c with a single rounding */
static inline f32b
sf_fma(f32b a, f32b b, f32b c)
{
	uint32_t sa, sb, sc, ma, mb, mc;
	int32_t ea, eb, ec;
	int za = !sf_unpack(a, &sa, &ea, &ma), zb = !sf_unpack(b, &sb, &eb, &mb), zc = !sf_unpack(c, &sc, &ec, &mc);

	if (sf_isnan(a) || sf_isnan(b) || sf_isnan(c)) {
		return SF_QNAN;
	}
	if (sf_isinf(a) || sf_isinf(b)) {
		if (za || zb) {
			return SF_QNAN;
		}
		f32b p = ((sa ^ sb) << 31) | SF_INF;
		return (sf_isinf(c) && ((c >> 31) != (p >> 31))) ? SF_QNAN : p;
	}
	if (sf_isinf(c)) {
		return c;
	}
	if (za || zb) {
		return zc ? ((((sa ^ sb) & sc)) << 31) : c;
	}
	{
		unsigned __int128 p = (unsigned __int128)((uint64_t)ma * (uint64_t)mb);
		int32_t ep = ea + eb - 150;       /* product = p * 2^(ep - 150) */

		if (zc) {
			return sf_pack64(sa ^ sb, ep, p);
		}
		return sf_add_core(sa ^ sb, ep, p, sc, ec, mc);
	}
}

/* a / b, correctly rounded */
static inline f32b
sf_div(f32b a, f32b b)
{
	uint32_t sa, sb, ma, mb;
	int32_t ea, eb;
	int za = !sf_unpack(a, &sa, &ea, &ma), zb = !sf_unpack(b, &sb, &eb, &mb);
	uint32_t s = sa ^ sb;

	if (sf_isnan(a) || sf_isnan(b)) {
		return SF_QNAN;
	}
	if (sf_isinf(a)) {
		return sf_isinf(b) ? SF_QNAN : ((s << 31) | SF_INF);
	}
	if (sf_isinf(b)) {
		return s << 31;
	}
	if (zb) {
		return za ? SF_QNAN : ((s << 31) | SF_INF);
	}
	if (za) {
		return s << 31;
	}
	{
		uint64_t num = (uint64_t)ma << 32;
		uint64_t q = num / mb;
		uint64_t rem = num % mb;

		return sf_pack64(s, ea - eb + 117, (((unsigned __int128)q) << 1) | (rem ? 1 : 0));
	}
}

/* integer -> float, correctly rounded */
static inline f32b
sf_from_i64(int64_t v)
{
	uint32_t s = v < 0;
	uint64_t mag = s ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;

	if (mag == 0) {
		return 0;
	}
	return sf_pack64(s, 150, (unsigned __int128)mag);
}

/* float -> signed fixed point with `frac` fractional bits, rounded half away from zero, saturating to int32 (the rasteriser's subpixel positions) */
static inline int32_t
sf_to_fixed(f32b a, int frac)
{
	uint32_t s = a >> 31;
	int32_t e = (int32_t)((a >> 23) & 0xff);
	uint64_t m;
	int sh;
	uint64_t r;

	if (e == 0 || sf_isnan(a)) {
		return 0;
	}
	if (e == 0xff) {
		return s ? INT32_MIN : INT32_MAX;
	}
	m = (a & 0x7fffff) | 0x800000;
	sh = e - 150 + frac;                    /* value = m * 2^sh */
	if (sh >= 0) {
		if (sh > 8) {
			return s ? INT32_MIN : INT32_MAX;
		}
		r = m << sh;
	} else if (-sh >= 40) {
		return 0;
	} else {
		r = (m + (1ULL << (-sh - 1))) >> -sh;
	}
	if (r > 0x7fffffffULL) {
		return s ? INT32_MIN : INT32_MAX;
	}
	return s ? -(int32_t)r : (int32_t)r;
}

/* clamp to [0, 1] (NaN -> 0) */
static inline f32b
sf_sat(f32b a)
{
	if (sf_isnan(a) || (a >> 31)) {
		return SF_ZERO;
	}
	if (a >= SF_ONE) {
		return SF_ONE;
	}
	return a;
}

/* float -> 8-bit unorm, round half up (the conversion the tile store uses) */
static inline uint8_t
sf_to_unorm8(f32b b)
{
	uint32_t e = (b >> 23) & 0xff;
	uint64_t mant;
	uint32_t shift;
	uint64_t r;

	if (b >> 31) {
		return 0;
	}
	if (e == 0xff) {
		return (b & 0x7fffff) ? 0 : 255;
	}
	if (e >= 127) {
		return 255;
	}
	if (e == 0) {
		return 0;
	}
	mant = (b & 0x7fffff) | 0x800000;
	shift = 23 + (127 - e);
	if (shift >= 62) {
		return 0;
	}
	r = (mant * 255 + (1ULL << (shift - 1))) >> shift;
	return r > 255 ? 255 : (uint8_t)r;
}

/* Mesa's 8-bit float immediate (agx_minifloat.h) as float bits */
static inline f32b
sf_minifloat(uint8_t imm)
{
	uint32_t sign = (imm & 0x80) ? 0x80000000u : 0;
	int exp = (imm & 0x70) >> 4;
	uint32_t man = imm & 0xF;
	uint32_t m;
	int e;

	if (exp) {
		m = man | 0x10;                 /* 1.xxxx * 16 = value mant * 2^(exp-7) */
		e = exp - 7;
	} else {
		m = man;
		e = -6;
		if (m == 0) {
			return sign;
		}
	}
	/* value = m * 2^e with m in [1, 31]: normalise */
	int msb = 4;
	while (msb >= 0 && !((m >> msb) & 1)) {
		msb--;
	}
	/* m = 1.xxx * 2^msb  =>  value = (m << (23 - msb)) * 2^(e + msb - 23) */
	return sign | ((uint32_t)(e + msb + 127) << 23) | (((m << (23 - msb)) & 0x7fffff));
}

/* IEEE half -> float bits (exact) */
static inline f32b
sf_h2f(uint16_t h)
{
	uint32_t s = ((uint32_t)h & 0x8000) << 16;
	uint32_t e = (h >> 10) & 0x1f;
	uint32_t m = h & 0x3ff;

	if (e == 0x1f) {
		return s | 0x7f800000u | (m << 13);
	}
	if (e == 0) {
		int sh = 0;

		if (m == 0) {
			return s;
		}
		while (!(m & 0x400)) {                  /* normalise the subnormal */
			m <<= 1;
			sh++;
		}
		return s | ((uint32_t)(113 - sh) << 23) | ((m & 0x3ff) << 13);
	}
	return s | ((e + 112) << 23) | (m << 13);
}

/* float bits -> IEEE half, round to nearest even */
static inline uint16_t
sf_f2h(f32b f)
{
	uint32_t s = (f >> 16) & 0x8000;
	int e = (f >> 23) & 0xff;
	uint32_t m = f & 0x7fffff;
	int ne;
	uint32_t r, rem;

	if (e == 0xff) {
		return (uint16_t)(s | 0x7c00 | (m ? 0x200 : 0));
	}
	ne = e - 127 + 15;
	if (ne >= 31) {
		return (uint16_t)(s | 0x7c00);
	}
	if (ne <= 0) {
		int sh;
		uint32_t half;

		if (ne < -10) {
			return (uint16_t)s;
		}
		m |= 0x800000;
		sh = 14 - ne;
		r = m >> sh;
		rem = m & ((1u << sh) - 1);
		half = 1u << (sh - 1);
		if (rem > half || (rem == half && (r & 1))) {
			r++;
		}
		return (uint16_t)(s | r);
	}
	r = ((uint32_t)ne << 10) | (m >> 13);
	rem = m & 0x1fff;
	if (rem > 0x1000 || (rem == 0x1000 && (r & 1))) {
		r++;                                    /* a carry into the exponent is the correct rounding */
	}
	return (uint16_t)(s | r);
}

#endif /* S8GFX_SF_H */
