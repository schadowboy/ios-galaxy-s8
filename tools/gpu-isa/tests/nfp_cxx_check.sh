#!/bin/bash
# the kernel glue is C++: compile the headers as C++ (clang++ -x c++, no exceptions/rtti, -mgeneral-regs-only -DKERNEL) with the S8FpWindow guard, in the flag variants
R=/mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
mkdir -p /tmp/nfp
cat > /tmp/nfp/cxx_probe.cpp <<'EOF'
#include <stdint.h>
#define S8_NATIVE_FP 1
#include "s8gfx_sf.h"
#include "s8gfx_nfp.h"
#include "s8gfx_rast.h"
#include "s8gfx_frag.h"
struct Obj { bool vert(uint32_t * out, f32b a, f32b b); };
bool Obj::vert(uint32_t * out, f32b a, f32b b)
{
	S8FpWindow fpw;
	f32b iw = sf_div(SF_ONE, a);
	*out = sf_fma(b, sf_mul(a, iw), sf_from_i64(3));
	fpw.restart();
	return true;
}
EOF
mkdir -p /tmp/nfp
for flags in "" "-DS8NFP_PREEMPT=0" "-DS8NFP_NATIVE_CONV=1" "-DS8_NATIVE_FP_ALWAYS"; do
  echo "== clang++ $flags"
  clang++ --target=aarch64-linux-gnu -x c++ -std=gnu++17 -fno-exceptions -fno-rtti -O2 -ffreestanding -mgeneral-regs-only -DKERNEL -Wall -Wno-unused-function -Wno-unused-variable $flags -I$R/core -c /tmp/nfp/cxx_probe.cpp -o /tmp/nfp/cxx_probe.o && echo ok
done
aarch64-linux-gnu-objdump -d /tmp/nfp/cxx_probe.o | grep -c "fmul\|fdiv\|fmadd"
