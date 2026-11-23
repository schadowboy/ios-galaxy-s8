#!/bin/bash
# end-to-end soft vs native on the real interpreter + rasteriser, under qemu-aarch64 user mode (default redirect set, then with the conversions too)
R=/mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
Q=/root/work/s8-qemu/qemu-user-build/qemu-aarch64
mkdir -p /tmp/nfp
for conv in 0 1; do
  echo "=== S8NFP_NATIVE_CONV=$conv"
  aarch64-linux-gnu-gcc -O2 -Wall -Wno-unused-function -Wno-unused-variable -static -DS8NFP_NATIVE_CONV=$conv -I$R/core -o /tmp/nfp/nfp_e2e$conv $R/tests/nfp_e2e.c || exit 1
  $Q /tmp/nfp/nfp_e2e$conv $R/data/corpus_comp.bin $R/data/corpus_f21.bin $R/data/corpus.bin $R/data/corpus3.bin $R/data/corpus4.bin
  echo "exit=$?"
done
