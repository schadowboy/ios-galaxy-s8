#!/bin/bash
# Agent E compile check: apply machine + devices to a scratch QEMU v10.2.1 tree and build it.
# Usage: check-build.sh [scratch-dir]   (default /root/work/s8-qemu/qemu-e-scratch)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SCR="${1:-/root/work/s8-qemu/qemu-e-scratch}"
bash "$HERE/../qemu/apply.sh" "$SCR"
mkdir -p "$SCR/build"
cd "$SCR/build"
[ -f build.ninja ] || ../configure --target-list=aarch64-softmmu --disable-docs --disable-werror
ninja -j6 qemu-system-aarch64
ls -l qemu-system-aarch64
