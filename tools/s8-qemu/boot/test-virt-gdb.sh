#!/bin/bash
# gdb evidence on the scratch virt run (run test-virt.sh first so $OUT has the blobs).
# usage: test-virt-gdb.sh <gdb-script>
set -euo pipefail
OUT=${OUT:-/root/work/s8-qemu/boot-out-virt}
pkill -f "qemu-system-aarch64.*-gdb" || true
qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a53 -smp 1 -m 4G -nographic -serial file:$OUT/gdb-serial.log -no-reboot -S -gdb tcp::1234 \
  -device loader,file=$OUT/payload.bin,addr=0x44000000,force-raw=on \
  -device loader,file=$OUT/loader_qemu.bin,addr=0x40200000,force-raw=on \
  -device loader,addr=0x40200000,cpu-num=0 > $OUT/gdb-qemu.out 2>&1 < /dev/null &
sleep 2
timeout 120 gdb-multiarch -q -batch -x "$1" || true
pkill -f "qemu-system-aarch64.*-gdb" || true
