#!/bin/bash
# M0: assemble tests/m0.S, boot it on the s8-a5xelte machine, print the PL011 output.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
Q=/root/work/s8-qemu/qemu-build/qemu-system-aarch64
T=/root/work/s8-qemu/m0
mkdir -p $T
aarch64-linux-gnu-gcc -nostdlib -static -Wl,-T,$HERE/tests/m0.ld -o $T/m0.elf $HERE/tests/m0.S
aarch64-linux-gnu-objcopy -O binary $T/m0.elf $T/m0.bin
timeout 30 $Q -machine s8-a5xelte -cpu cortex-a53 -smp 4 -m 2560M -display none -monitor none \
  -serial stdio -device loader,file=$T/m0.bin,addr=0x80080000,cpu-num=0 "$@"
echo "qemu exit code: $?"
