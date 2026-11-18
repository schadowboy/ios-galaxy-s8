#!/bin/bash
# Build devices/test/fbtest.c and boot it on the s8-a5xelte machine.
#   build-run.sh [qemu-binary] [extra qemu args...]
# Default: headless with QMP socket /tmp/s8e-qmp.sock, serial on stdout.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
Q=${1:-/root/work/s8-qemu/qemu-e-scratch/build/qemu-system-aarch64}
[ $# -gt 0 ] && shift
T=/root/work/s8-qemu/e-test
mkdir -p $T
cat > $T/start.S <<'S'
.section .text.boot
.global _start
_start:
    mrs x0, mpidr_el1
    and x0, x0, #3
    cbnz x0, 1f
    ldr x1, =0x80200000
    mov sp, x1
    bl main
1:  wfi
    b 1b
S
cat > $T/fb.ld <<'L'
ENTRY(_start)
SECTIONS { . = 0x80080000; .text : { *(.text.boot) *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) } }
L
aarch64-linux-gnu-gcc -O1 -ffreestanding -nostdlib -static -mgeneral-regs-only -Wl,-T,$T/fb.ld -o $T/fbtest.elf $T/start.S "$HERE/fbtest.c"
aarch64-linux-gnu-objcopy -O binary $T/fbtest.elf $T/fbtest.bin
rm -f /tmp/s8e-qmp.sock
exec $Q -machine s8-a5xelte -cpu cortex-a53 -smp 4 -m 2560M -display none -monitor none -serial stdio \
  -qmp unix:/tmp/s8e-qmp.sock,server=on,wait=off \
  -device loader,file=$T/fbtest.bin,addr=0x80080000,cpu-num=0,force-raw=on "$@"
