#!/bin/bash
# Boot Agent D's payload on s8-a5xelte with the exynos-ufs disk and dump the kernel console ring.
# Usage: boot-kernel-ufs.sh [seconds=40] [image=/root/work/s8-qemu/s8qemu-ufs0.img]
SECS=${1:-40}
IMG=${2:-/root/work/s8-qemu/s8qemu-ufs0.img}
O=/root/work/s8-qemu/boot-out
Q=/root/work/s8-qemu/qemu-build/qemu-system-aarch64
rm -f /tmp/s8mon.sock /tmp/klog.bin
$Q -machine s8-a5xelte -cpu cortex-a53 -smp 1 -m 2560M \
  -display none -monitor unix:/tmp/s8mon.sock,server,nowait -serial file:/root/work/s8-qemu/boot-serial.txt \
  -d guest_errors,unimp -D /root/work/s8-qemu/boot-trace.txt \
  -drive file=$IMG,if=none,id=ufs0,format=raw -global exynos-ufs.drive=ufs0 \
  -device loader,file=$O/payload.bin,addr=0x89000000,force-raw=on \
  -device loader,file=$O/loader_qemu.bin,addr=0x80080000,force-raw=on \
  -device loader,addr=0x80080000,cpu-num=0 &
QP=$!
sleep "$SECS"
python3 - <<'EOF'
import socket, time
s = socket.socket(socket.AF_UNIX); s.connect('/tmp/s8mon.sock'); time.sleep(0.5); s.recv(65536)
s.send(b'pmemsave 0x92180000 0x80000 "/tmp/klog.bin"\n'); time.sleep(2); s.send(b'quit\n')
EOF
wait $QP 2>/dev/null
strings -n 6 /tmp/klog.bin | grep -i -E 'ufs|root|mount|panic' | head -60
