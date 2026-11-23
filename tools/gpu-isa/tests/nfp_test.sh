#!/bin/bash
# Build tools/gpu-isa/tests/nfp_test.c for aarch64, static, run under qemu-aarch64 user mode.   usage: nfp_test.sh [N] [extra gcc flags...]
T=/mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa/tests
Q=/root/work/s8-qemu/qemu-user-build/qemu-aarch64
N=${1:-2000000}
shift
for i in $(seq 1 45); do [ -x $Q ] && break; echo "waiting for qemu-aarch64 ($i)"; sleep 20; done
[ -x $Q ] || { echo "no qemu-aarch64"; exit 2; }
mkdir -p /tmp/nfp
aarch64-linux-gnu-gcc -O2 -Wall -Wno-unused-function -static "$@" -o /tmp/nfp/nfp_test $T/nfp_test.c || exit 1
time $Q /tmp/nfp/nfp_test $N
echo "exit=$?"
