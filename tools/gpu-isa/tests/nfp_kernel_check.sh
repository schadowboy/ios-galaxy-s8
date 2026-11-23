#!/bin/bash
# compile nfp_sizes.c as the kernel TU would be compiled (-mgeneral-regs-only -DKERNEL, clang and gcc), check that no FP instruction is emitted outside
# the asm blocks (every FP instruction must be inside the w_nfp_*/w_begin/w_end/selftest bodies; the soft wrappers must contain none), list instruction counts
R=/mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
mkdir -p /tmp/nfp
for cc in "clang --target=aarch64-linux-gnu" "aarch64-linux-gnu-gcc"; do
  tag=$(echo $cc | cut -c1-5)
  echo "=== $cc"
  $cc -O2 -ffreestanding -fno-builtin -mgeneral-regs-only -DKERNEL -Wall -Wno-unused-function -Wno-unused-variable -I$R/core -c $R/tests/nfp_sizes.c -o /tmp/nfp/sizes_$tag.o || exit 1
  aarch64-linux-gnu-objdump -d /tmp/nfp/sizes_$tag.o > /tmp/nfp/sizes_$tag.dis
  # instruction count per function
  awk '/^[0-9a-f]+ <w_/ {name=$2; n[name]=0; order[++k]=name} /^ +[0-9a-f]+:/ {if (name!="") n[name]++} END {for (i=1;i<=k;i++) printf "%-24s %d\n", order[i], n[order[i]]}' /tmp/nfp/sizes_$tag.dis
  echo "--- FP/NEON mnemonics outside w_nfp_*, w_begin, w_end, w_selftest:"
  awk '/^[0-9a-f]+ <.*>:/ {name=$2} /^ +[0-9a-f]+:/ { if (name !~ /w_nfp_|w_begin|w_end|w_selftest/) { m=$3 FS $4; print name, $0 } }' /tmp/nfp/sizes_$tag.dis | grep -E "\s(fmov|fadd|fsub|fmul|fdiv|fmadd|fcvt|scvtf|ucvtf|ldp\s+q|stp\s+q|ldr\s+[sdq][0-9]|str\s+[sdq][0-9]|movi|dup|ins|umov|fabs|fneg)" | head
  echo "(none above = clean)"
done
echo "=== native sequences (clang)"
for f in w_nfp_mul w_nfp_add w_nfp_fma w_nfp_div w_nfp_from_i64 w_nfp_to_fixed w_nfp_to_unorm8 w_nfp_h2f w_nfp_f2h w_begin w_end; do
  echo "--- $f"; awk -v f="<$f>:" '$2==f {p=1; next} /^$/ {p=0} p' /tmp/nfp/sizes_clang.dis | cut -f3-
done
