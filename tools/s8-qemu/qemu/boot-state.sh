#!/bin/bash
# Boot Agent D's payload, let it run N seconds (default 10), then dump CPU registers via the monitor.
O=/root/work/s8-qemu/boot-out
N=${1:-10}
( sleep $N; echo "info registers"; echo "x/8i \$pc"; sleep 1; echo quit ) | \
timeout 60 /root/work/s8-qemu/qemu-build/qemu-system-aarch64 -machine s8-a5xelte -cpu cortex-a53 -smp 1 -m 2560M \
  -display none -monitor stdio -serial null \
  -d guest_errors,unimp -D /root/work/s8-qemu/boot-trace.txt \
  -device loader,file=$O/payload.bin,addr=0x89000000,force-raw=on \
  -device loader,file=$O/loader_qemu.bin,addr=0x80080000,force-raw=on \
  -device loader,addr=0x80080000,cpu-num=0 2>&1 | grep -E "^(.?PC|X0|X1|X2|X3|X28|X29|X30|SP|PSTATE|ELR|ESR|FAR|VBAR|SCTLR|   0x|0x)" | head -30
echo --- trace; head -20 /root/work/s8-qemu/boot-trace.txt | cut -c1-200
