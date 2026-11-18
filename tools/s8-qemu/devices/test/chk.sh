#!/bin/bash
H=/mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/s8-qemu
bash $H/qemu/apply.sh /root/work/s8-qemu/qemu-e-scratch > /dev/null
cd /root/work/s8-qemu/qemu-e-scratch/build
ninja qemu-system-aarch64 > /tmp/n.log 2>&1
echo "ninja rc=$?"
grep -n 'error' /tmp/n.log | head
./qemu-system-aarch64 -machine help | grep -i 'a5xelte'
