/* Kernel-configuration compile check and instruction-count listing of the native vs soft primitives.
 * clang/gcc --target aarch64 -O2 -ffreestanding -mgeneral-regs-only -DKERNEL -c nfp_sizes.c   (see nfp_kernel_check.sh) */
#include <stdint.h>
#define S8_NATIVE_FP 1
#include "s8gfx_sf.h"
#include "s8gfx_nfp.h"
#include "s8gfx_rast.h"
#include "s8gfx_frag.h"
#include "s8gfx_tex.h"
#include "s8gfx_stream.h"

#define NOINL __attribute__((noinline))

/* native */
NOINL f32b w_nfp_mul(f32b a, f32b b) { return nfp_mul(a, b); }
NOINL f32b w_nfp_add(f32b a, f32b b) { return nfp_add(a, b); }
NOINL f32b w_nfp_fma(f32b a, f32b b, f32b c) { return nfp_fma(a, b, c); }
NOINL f32b w_nfp_div(f32b a, f32b b) { return nfp_div(a, b); }
NOINL f32b w_nfp_from_i64(int64_t v) { return nfp_from_i64(v); }
NOINL int32_t w_nfp_to_fixed(f32b a) { return nfp_to_fixed(a, 8); }
NOINL uint8_t w_nfp_to_unorm8(f32b a) { return nfp_to_unorm8(a); }
NOINL f32b w_nfp_h2f(uint16_t h) { return nfp_h2f(h); }
NOINL uint16_t w_nfp_f2h(f32b f) { return nfp_f2h(f); }
/* soft (pre-remap definitions are only reachable through the s8nfp_sel_* wrappers' else branch; compile the soft ones by name via a TU without the macros: see below) */
#undef sf_mul
#undef sf_add
#undef sf_fma
#undef sf_div
#undef sf_from_i64
#undef sf_to_fixed
#undef sf_to_unorm8
#undef sf_h2f
#undef sf_f2h
#undef sf_sub
NOINL f32b w_soft_mul(f32b a, f32b b) { return sf_mul(a, b); }
NOINL f32b w_soft_add(f32b a, f32b b) { return sf_add(a, b); }
NOINL f32b w_soft_fma(f32b a, f32b b, f32b c) { return sf_fma(a, b, c); }
NOINL f32b w_soft_div(f32b a, f32b b) { return sf_div(a, b); }
NOINL f32b w_soft_from_i64(int64_t v) { return sf_from_i64(v); }
NOINL int32_t w_soft_to_fixed(f32b a) { return sf_to_fixed(a, 8); }
NOINL uint8_t w_soft_to_unorm8(f32b a) { return sf_to_unorm8(a); }
NOINL f32b w_soft_h2f(uint16_t h) { return sf_h2f(h); }
NOINL uint16_t w_soft_f2h(f32b f) { return sf_f2h(f); }

NOINL void w_enable(int on) { s8nfp_enable(on); }
NOINL void w_begin(struct s8_fpsave * s) { s8_fp_begin(s); }
NOINL void w_end(struct s8_fpsave * s) { s8_fp_end(s); }
NOINL int w_selftest(void) { return s8nfp_selftest(); }
