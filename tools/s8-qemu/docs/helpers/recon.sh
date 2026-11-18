#!/bin/bash
# Reconstruct the final text of patch-created kernel source files by applying all xnu patches in order
# (only the listed files; files that exist only as a modification of Apple's xnu cannot be rebuilt).
# Output tree: /root/qa-src (WSL). Log: /root/qa-apply.log
P=/mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress/xnu/patches
rm -rf /root/qa-src; mkdir -p /root/qa-src; cd /root/qa-src
INC=""
for f in bsd/dev/sweet_ufs.c bsd/dev/a5xelte_touch.c bsd/dev/a5xelte_keys.c iokit/Kernel/arm/S8Agx.cpp iokit/Kernel/arm/S8Mali.cpp iokit/Kernel/arm/SweetCPU.cpp iokit/Kernel/arm/SweetPlatformExpert.cpp iokit/Kernel/arm/SweetBattery.cpp iokit/Kernel/arm/SweetFramebuffer.cpp osfmk/arm64/sweet_early.c osfmk/kern/s8_acpm.c osfmk/kern/s8_mali.c osfmk/kern/s8_mali_drv.c pexpert/pexpert/arm64/SWEET.h pexpert/pexpert/arm64/A5XELTE.h pexpert/arm/pe_serial.c pexpert/arm/pe_fiq.c; do INC="$INC --include=$f"; done
for p in $(ls $P/*.patch | sort); do
  if /usr/bin/git apply --reject --whitespace=nowarn $INC $p >/dev/null 2>&1; then echo "ok $(basename $p)"; else echo "FAIL $(basename $p)"; fi
done > /root/qa-apply.log
find . -type f -not -name "*.rej" | xargs wc -l; find . -name "*.rej" | head -30
grep -c FAIL /root/qa-apply.log
