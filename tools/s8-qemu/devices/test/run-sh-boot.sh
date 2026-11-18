#!/bin/bash
# Smoke test: run.sh with the scratch QEMU, a dummy 64 MiB base image, existing payload; 25 s headless.
export PATH=$PATH:/root/work/s8-qemu/qemu-e-scratch/build
H="$(cd "$(dirname "$0")/../.." && pwd)"
ls /root/work/s8-qemu/boot-out/qemu-args.txt || exit 1
[ -e /root/work/s8-qemu/e-test/dummy.img ] || truncate -s 64M /root/work/s8-qemu/e-test/dummy.img
S8_QEMU=/root/work/s8-qemu/qemu-e-scratch/build/qemu-system-aarch64 S8_DISK_BASE=/root/work/s8-qemu/e-test/dummy.img \
  timeout 25 bash $H/run.sh --no-payload --headless --fresh --gdb 2>&1 | cut -c1-300 | head -40
echo "--- ram log via QMP (VM already gone, expect MISSING message)"
bash $H/read-log.sh --ram 2>&1 | head -5
