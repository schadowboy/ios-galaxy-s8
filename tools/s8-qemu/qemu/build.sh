#!/bin/bash
# Idempotent: clone QEMU v10.2.1, apply s8-qemu sources, build aarch64-softmmu in /root/work/s8-qemu.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
W=/root/work/s8-qemu
mkdir -p $W
[ -d $W/qemu-src ] || git clone --depth 1 --branch v10.2.1 https://gitlab.com/qemu-project/qemu.git $W/qemu-src
bash "$HERE/apply.sh" $W/qemu-src
mkdir -p $W/qemu-build
cd $W/qemu-build
[ -f build.ninja ] || $W/qemu-src/configure --target-list=aarch64-softmmu --disable-docs --disable-werror
ninja -j10 qemu-system-aarch64
ls -l $W/qemu-build/qemu-system-aarch64
