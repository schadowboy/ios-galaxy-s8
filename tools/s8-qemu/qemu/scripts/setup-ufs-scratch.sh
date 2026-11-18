#!/bin/bash
# Agent C: fetch + build a scratch QEMU 10.2.1 (aarch64-softmmu only) with the exynos-ufs device,
# in /root/work/s8-qemu/qemu-ufs-scratch. Official tarball from download.qemu.org, sha256 checked.
# Run as root inside WSL:  wsl -u root -e bash /mnt/c/.../tools/s8-qemu/qemu/scripts/setup-ufs-scratch.sh
set -e
W=/root/work/s8-qemu/qemu-ufs-scratch
mkdir -p $W
cd $W
apt-get install -y ninja-build libglib2.0-dev libpixman-1-dev python3-venv python3-pip flex bison pkg-config xz-utils curl gcc-aarch64-linux-gnu >/dev/null
if [ ! -f qemu-10.2.1.tar.xz ]; then
  curl -fsSL -o qemu-10.2.1.tar.xz https://download.qemu.org/qemu-10.2.1.tar.xz
fi
sha256sum qemu-10.2.1.tar.xz
[ -d qemu-10.2.1 ] || tar xf qemu-10.2.1.tar.xz
echo setup done
