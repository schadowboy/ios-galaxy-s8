#!/bin/bash
cd /mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/gpu-isa/tests/nfp
clang --version | head -1
echo "== clang -mgeneral-regs-only"
clang --target=aarch64-linux-gnu -O2 -mgeneral-regs-only -ffreestanding -c probe.c -o /tmp/probe_clang.o && aarch64-linux-gnu-objdump -d /tmp/probe_clang.o | head -30
echo "== gcc -mgeneral-regs-only"
aarch64-linux-gnu-gcc -O2 -mgeneral-regs-only -ffreestanding -c probe.c -o /tmp/probe_gcc.o && aarch64-linux-gnu-objdump -d /tmp/probe_gcc.o | head -30
