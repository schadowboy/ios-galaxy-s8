#!/bin/bash
# compile check of wifi_w2.c with the loader's own flags (shim/dreamlte/build.sh); also checks for ABS64 relocations
W=/mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress
CC=aarch64-linux-gnu-gcc
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-tree-loop-distribute-patterns -march=armv8-a -Wall -Wextra -Werror"
mkdir -p /tmp/w2chk
$CC $CFLAGS -c $W/tools/wifi-w2/wifi_w2.c -o /tmp/w2chk/wifi_w2.o && echo COMPILE_OK
echo "ABS64 relocs: $(aarch64-linux-gnu-readelf -r /tmp/w2chk/wifi_w2.o | grep -c ABS64)"
aarch64-linux-gnu-nm /tmp/w2chk/wifi_w2.o
aarch64-linux-gnu-objdump -d /tmp/w2chk/wifi_w2.o | grep -c "bl.*memset\|bl.*memcpy"
if [ -n "$1" ]; then
  # full loader sources compile (after the append): smp_probe.c and loader.c
  cd $W/shim/dreamlte
  $CC $CFLAGS -c smp_probe.c -o /tmp/w2chk/smp_probe.o && echo SMP_PROBE_OK
  $CC $CFLAGS -c loader.c -o /tmp/w2chk/loader.o && echo LOADER_OK
  echo "ABS64 relocs smp_probe: $(aarch64-linux-gnu-readelf -r /tmp/w2chk/smp_probe.o | grep -c ABS64)"
fi
