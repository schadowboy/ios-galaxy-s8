#!/bin/bash
# Build the s8-qemu boot stage.
# Env: LOADER_BASE (0x80080000), PAYLOAD_PA (0x84000000), UART_BASE (0 = silent), OUT (./out),
#      XNU_RAM_BASE / XNU_RAM_END (default 0x880000000 / 0x900000000; scratch virt tests override)
set -euo pipefail
cd "$(dirname "$0")"
LOADER_BASE=${LOADER_BASE:-0x80080000}
PAYLOAD_PA=${PAYLOAD_PA:-0x84000000}
UART_BASE=${UART_BASE:-0}
OUT=${OUT:-out}
CC=aarch64-linux-gnu-gcc
DEFS="-DPAYLOAD_PA=${PAYLOAD_PA}UL -DUART_BASE=${UART_BASE}UL"
[ -n "${XNU_RAM_BASE:-}" ] && DEFS="$DEFS -DXNU_RAM_BASE=${XNU_RAM_BASE}UL -DXNU_RAM_END=${XNU_RAM_END}UL"
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-tree-loop-distribute-patterns -march=armv8-a -Wall -Wextra -Werror $DEFS"
mkdir -p "$OUT"
$CC $CFLAGS -c start.S -o $OUT/start.o
$CC $CFLAGS -c loader_qemu.c -o $OUT/loader_qemu.o
$CC $CFLAGS -static -Wl,--defsym=LOADER_BASE=$LOADER_BASE -T loader_qemu.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N $OUT/start.o $OUT/loader_qemu.o -o $OUT/loader_qemu.elf
aarch64-linux-gnu-objcopy -O binary $OUT/loader_qemu.elf $OUT/loader_qemu.bin
echo "$OUT/loader_qemu.bin: $(stat -c %s $OUT/loader_qemu.bin) bytes, base $LOADER_BASE, payload $PAYLOAD_PA, uart $UART_BASE"
