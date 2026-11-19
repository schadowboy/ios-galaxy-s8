#!/bin/bash
# Build the s8-qemu boot blobs (run in WSL).
#   make-payload.sh [extra boot-args...]
# Env: KERNEL (kernel Mach-O), OUT (/root/work/s8-qemu/boot-out), QCPUS (1), DRAM_SIZE (0x20000000, ADT kernel RAM at 0x880000000),
#      LOADER_BASE (0x80080000), PAYLOAD_PA (0x89000000), UART_BASE (0 = silent), CANVAS (1440x2960)
# Outputs in $OUT: dreamlte-qemu.adt, payload.bin (SWEETXNU v1: kernel segments with chained fixups resolved + ADT + cmdline),
#                  loader_qemu.bin / .elf, qemu-args.txt
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
tools=$(cd "$here/../.." && pwd)
KERNEL=${KERNEL:-/mnt/c/github/ios-s8/upstream/xnu/out-v100/xnu-kernel/kernel.development.a5xelte}
OUT=${OUT:-/root/work/s8-qemu/boot-out}
QCPUS=${QCPUS:-1}
DRAM_SIZE=${DRAM_SIZE:-0x80000000}
DRAM_BASE=${DRAM_BASE:-0x880000000}
export XNU_RAM_BASE=${XNU_RAM_BASE:-$DRAM_BASE}
export XNU_RAM_END=${XNU_RAM_END:-$(printf "0x%x" $((DRAM_BASE + 0x80000000)))}
export LOADER_BASE=${LOADER_BASE:-0x80080000}
export PAYLOAD_PA=${PAYLOAD_PA:-0x89000000}
export UART_BASE=${UART_BASE:-0}
CANVAS=${CANVAS:-1440x2960}
mkdir -p "$OUT"
# Phone set (build-v138.sh + make-boot-kernel.sh) minus hardware-poking args: s8dvfs s8dvfs_ms dvfsch s8gpudvfs s8gputmu mngsfreq s8bigboot
# s8recov (PMU reset to recovery) and a53freq/mngsexp. Kept: s8rtc, vm_compression_limit, s8mali_drv/s8agx*, sweetfb_*, ufsroot.
args="debug=0xc serial=3 cpus=$QCPUS keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 ufsroot=USERDATA \
unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 \
a5touchprobe=0 s8touch=0 a5fb_keep_on=1 a5fb_noconsole=1 sweetfb_native=1 \
vm_compression_limit=524288 s8rtc=$(date +%s) s8mali_drv=1 s8panicwait=30 sweetfb_async=1 s8agx=1 s8agxskip=59 $*"
python3 -I "$here/mkadt_qemu.py" "$OUT/dreamlte-qemu.adt" --cpus "$QCPUS" --dram-size "$DRAM_SIZE" --dram-base "$DRAM_BASE" --canvas "$CANVAS"
python3 "$tools/mkpayload.py" "$KERNEL" "$OUT/dreamlte-qemu.adt" "$OUT/payload.bin" --cmdline "$args" | tee "$OUT/payload.info"
OUT="$OUT" "$here/build.sh"
psize=$(stat -c %s "$OUT/payload.bin")
cat > "$OUT/qemu-args.txt" <<EOT
-device loader,file=$OUT/payload.bin,addr=$PAYLOAD_PA,force-raw=on -device loader,file=$OUT/loader_qemu.bin,addr=$LOADER_BASE,force-raw=on -device loader,addr=$LOADER_BASE,cpu-num=0
EOT
echo
echo "== load map =="
echo "loader_qemu  $LOADER_BASE  (text+bss+64KiB stack; CPU0 start PC; low bank)"
echo "payload      $PAYLOAD_PA  size $psize (SWEETXNU; consumed by loader, low bank, must stay below 0xbc800000)"
echo "kernel RAM   $DRAM_BASE + $DRAM_SIZE (loader copies segments to phys_base+(vmaddr-virt_base), ADT right after kernel_end, boot_args on the next 4K page)"
echo "see $OUT/payload.info for virt_base/entry/kernel_end"
echo "== qemu fragment =="
cat "$OUT/qemu-args.txt"
