#!/bin/bash
# Build and run the exynos-ufs bare-metal test. Run in WSL (root). Usage: run-ufs-test.sh [qemu-system-aarch64]
set -e
T=$(cd "$(dirname "$0")" && pwd)
QEMU=${1:-/root/work/s8-qemu/qemu-ufs-scratch/build/qemu-system-aarch64}
O=/root/work/s8-qemu/qemu-ufs-scratch/test
mkdir -p $O
cat > $O/link.ld <<'EOF'
ENTRY(_start)
SECTIONS { . = 0x80000000; .text : { *(.text.boot) *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) } }
EOF
aarch64-linux-gnu-gcc -O1 -ffreestanding -nostdlib -fno-builtin -mgeneral-regs-only -mstrict-align \
  -fno-pic -no-pie -Wl,-T,$O/link.ld -o $O/ufs-test.elf $T/ufs-test.c
aarch64-linux-gnu-objcopy -O binary $O/ufs-test.elf $O/ufs-test.bin
# 64 MiB image, 4096-byte blocks with a known pattern (block b, byte j = b*131 + j*7 + (b>>8))
python3 - $O/ufs.img <<'EOF'
import sys
n = 16384
with open(sys.argv[1], 'wb') as f:
    for b in range(n):
        f.write(bytes(((b * 131 + j * 7 + (b >> 8)) & 0xff) for j in range(4096)))
EOF
$QEMU -M s8-ufs-test -display none -serial stdio -monitor none -semihosting $EXTRA \
  -drive file=$O/ufs.img,if=none,id=ufs0,format=raw -global exynos-ufs.drive=ufs0 \
  -device loader,file=$O/ufs-test.bin,addr=0x80000000,cpu-num=0 | tee $O/run.log
# host-side check of what the guest wrote: 16 blocks at last-1024, first of them rewritten by WRITE(16) with 0x5a
python3 - $O/ufs.img <<'EOF'
import sys
n = 16384
wl = n - 1 - 1024
d = open(sys.argv[1], 'rb').read()
blk0 = d[wl * 4096:(wl + 1) * 4096]
assert blk0 == bytes([0x5a]) * 4096, "WRITE(16) block mismatch in image file"
rest = d[(wl + 1) * 4096:(wl + 16) * 4096]
exp = bytes((0xa5 ^ (j * 3)) & 0xff for j in range(4096, 4096 * 16))
assert rest == exp, "WRITE(10) data mismatch in image file"
print("host check: backing file contains the guest writes")
EOF
