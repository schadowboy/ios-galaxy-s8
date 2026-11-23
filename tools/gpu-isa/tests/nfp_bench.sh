#!/bin/bash
R=/mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa
Q=/root/work/s8-qemu/qemu-user-build/qemu-aarch64
mkdir -p /tmp/nfp
aarch64-linux-gnu-gcc -O2 -static -o /tmp/nfp/nfp_bench $R/tests/nfp_bench.c || exit 1
$Q /tmp/nfp/nfp_bench
