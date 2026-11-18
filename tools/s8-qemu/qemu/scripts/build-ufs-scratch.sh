#!/bin/bash
# Build a scratch aarch64-softmmu QEMU (copy of /root/work/s8-qemu/qemu-src) containing exynos-ufs
# and the s8-ufs-test machine, in /root/work/s8-qemu/qemu-ufs-scratch. Run as root in WSL.
set -e
W=/root/work/s8-qemu/qemu-ufs-scratch
T=/mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/tools/s8-qemu/qemu
mkdir -p $W
if [ ! -d $W/src ]; then
  rsync -a --exclude=.git /root/work/s8-qemu/qemu-src/ $W/src/
fi
bash $T/scripts/apply-ufs.sh $W/src --test-machine
mkdir -p $W/build
cd $W/build
if [ ! -f build.ninja ]; then
  ../src/configure --target-list=aarch64-softmmu --disable-docs --disable-tools --disable-user \
    --disable-guest-agent --disable-werror --disable-slirp --disable-vnc --disable-gtk --disable-sdl \
    --disable-opengl --disable-curses >configure.log 2>&1
fi
ninja qemu-system-aarch64 >ninja.log 2>&1
ls -la qemu-system-aarch64
