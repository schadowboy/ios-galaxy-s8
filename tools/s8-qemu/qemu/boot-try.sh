#!/bin/bash
# Boot Agent D's payload on s8-a5xelte for 20 s and show guest errors / unimplemented accesses.
O=/root/work/s8-qemu/boot-out
timeout 20 /root/work/s8-qemu/qemu-build/qemu-system-aarch64 -machine s8-a5xelte -cpu cortex-a53 -smp 1 -m 2560M \
  -display none -monitor none -serial file:/root/work/s8-qemu/boot-serial.txt \
  -d guest_errors,unimp,int -D /root/work/s8-qemu/boot-trace.txt \
  -device loader,file=$O/payload.bin,addr=0x89000000,force-raw=on \
  -device loader,file=$O/loader_qemu.bin,addr=0x80080000,force-raw=on \
  -device loader,addr=0x80080000,cpu-num=0 "$@"
echo "exit $?"
grep -vc . /root/work/s8-qemu/boot-trace.txt
head -40 /root/work/s8-qemu/boot-trace.txt | cut -c1-200
echo ...; tail -25 /root/work/s8-qemu/boot-trace.txt | cut -c1-200
