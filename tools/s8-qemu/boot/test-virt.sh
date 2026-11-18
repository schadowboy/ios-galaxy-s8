#!/bin/bash
# Scratch validation on stock `qemu-system-aarch64 -M virt` (NOT the s8 machine): proves loader_qemu runs to the kernel jump and the kernel
# executes. virt RAM is at 0x40000000, so the load map is shifted: loader 0x40200000, payload 0x44000000, kernel RAM bank 0xb0000000.
# usage: test-virt.sh [secs]   -> $OUT/virt-*.log
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/root/work/s8-qemu/boot-out-virt}
export OUT
export DRAM_BASE=0xb0000000 LOADER_BASE=0x40200000 PAYLOAD_PA=0x44000000 UART_BASE=0x09000000 QCPUS=${QCPUS:-1}
"$here/make-payload.sh" > "$OUT.make.log" 2>&1 || { cat "$OUT.make.log"; exit 1; }
timeout ${1:-20} qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a53 -smp 1 -m 4G -nographic -serial file:$OUT/virt-serial.log \
  -d int,cpu_reset,guest_errors,unimp -D $OUT/virt-qemu.log -no-reboot \
  -device loader,file=$OUT/payload.bin,addr=$PAYLOAD_PA,force-raw=on \
  -device loader,file=$OUT/loader_qemu.bin,addr=$LOADER_BASE,force-raw=on \
  -device loader,addr=$LOADER_BASE,cpu-num=0 || true
cat $OUT/virt-serial.log
