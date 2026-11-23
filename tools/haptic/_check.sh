#!/bin/bash
# compile check of haptic.c with the loader's own flags (shim/dreamlte/build.sh); also checks for ABS64 relocations
# usage: _check.sh        -> haptic.c alone
#        _check.sh full   -> also smp_probe.c (after _append.sh) and loader.c
W=/mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress
CC=aarch64-linux-gnu-gcc
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-tree-loop-distribute-patterns -march=armv8-a -Wall -Wextra -Werror"
mkdir -p /tmp/hpchk
$CC $CFLAGS -c $W/tools/haptic/haptic.c -o /tmp/hpchk/haptic.o && echo COMPILE_OK
echo "ABS64 relocs: $(aarch64-linux-gnu-readelf -r /tmp/hpchk/haptic.o | grep -c ABS64)"
aarch64-linux-gnu-nm /tmp/hpchk/haptic.o
echo "memset/memcpy calls: $(aarch64-linux-gnu-objdump -d /tmp/hpchk/haptic.o | grep -c 'bl.*mem')"
if [ -n "$1" ]; then
  cd $W/shim/dreamlte
  $CC $CFLAGS -c smp_probe.c -o /tmp/hpchk/smp_probe.o && echo SMP_PROBE_OK
  $CC $CFLAGS -c loader.c -o /tmp/hpchk/loader.o && echo LOADER_OK
  echo "ABS64 relocs smp_probe: $(aarch64-linux-gnu-readelf -r /tmp/hpchk/smp_probe.o | grep -c ABS64)"
  echo "ABS64 relocs loader: $(aarch64-linux-gnu-readelf -r /tmp/hpchk/loader.o | grep -c ABS64)"
fi
