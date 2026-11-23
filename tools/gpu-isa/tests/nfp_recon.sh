#!/bin/bash
Q=/root/work/s8-qemu/qemu-user-build
ls $Q/contrib/plugins/*.so $Q/tests/tcg/plugins/*.so 2>&1 | head
$Q/qemu-aarch64 --version | head -1
$Q/qemu-aarch64 -plugin help 2>&1 | head -3
find /root/work/s8-qemu -name "libinsn.so" 2>/dev/null | head
echo "--- begin disasm"
awk -v f="<w_begin>:" '$2==f {p=1; next} /^$/ {p=0} p' /tmp/nfp/sizes_clang.dis | cut -f3-
