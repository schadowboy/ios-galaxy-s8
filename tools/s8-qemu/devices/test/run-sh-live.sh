#!/bin/bash
# run.sh live: read the RAM klog via QMP and attach gdb with kernel symbols (scratch QEMU, no UFS).
export PATH=$PATH:/root/work/s8-qemu/qemu-e-scratch/build
H="$(cd "$(dirname "$0")/../.." && pwd)"
S8_QEMU=/root/work/s8-qemu/qemu-e-scratch/build/qemu-system-aarch64 S8_DISK_BASE=/root/work/s8-qemu/e-test/dummy.img \
  timeout 45 bash $H/run.sh --no-payload --headless --fresh --gdb > /tmp/live.out 2>&1 &
sleep 15
echo "--- read-log --ram"
bash $H/read-log.sh --ram 2>&1 | head -15
echo "--- gdb"
timeout 60 bash $H/gdb.sh -- -batch -ex 'info registers pc' -ex 'x/2i $pc' -ex 'info symbol $pc' 2>&1 | tail -12
wait
