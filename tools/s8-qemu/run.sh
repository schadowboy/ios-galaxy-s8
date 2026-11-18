#!/bin/bash
# s8-qemu one-command runner (run in WSL as root; works from the Windows checkout via /mnt/c).
#
#   tools/s8-qemu/run.sh [options] [-- extra-qemu-args]
#
#   --gdb            open a gdb stub on tcp::1234 (use gdb.sh); --gdb-wait also halts the CPUs at reset (-S)
#   --snapshot NAME  boot from internal snapshot NAME of the disk overlay if it exists (-loadvm).
#                    Save one while running:  devices/qmp.py hmp "savevm NAME"
#   --headless       no window (-display none); screenshots: devices/qmp.py screendump out.ppm
#   --crop           show only the centred 1125x2436 UI area of the 1440x2960 panel
#   --fresh          throw away the copy-on-write disk overlay first (the base image is never written)
#   --no-payload     do not rebuild the boot payload (boot/make-payload.sh)
#   --bootargs "..." extra kernel boot-args handed to make-payload.sh
#   --mem SIZE       machine RAM (default 2560M)   --smp N (default 4)
#
# Environment overrides: S8_DISK_BASE (raw userdata image), S8_QEMU (qemu-system-aarch64),
#   S8_WORK (default /root/work/s8-qemu), KERNEL (kernel Mach-O for make-payload.sh)
#
# Keys in the QEMU window: P power, H home, PageUp/PageDown volume; mouse = touch (left button).
# Logs: $S8_WORK/run/<timestamp>/{serial.log,qemu.log}; "latest" symlink; QMP socket $S8_WORK/run/qmp.sock.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
S8_WORK=${S8_WORK:-/root/work/s8-qemu}
QEMU=${S8_QEMU:-$S8_WORK/qemu-build/qemu-system-aarch64}
BASE=${S8_DISK_BASE:-/root/work/s8qemu-ufs0.img}   # make_gpt.py output (docs/platform.md 4.2)
export PATH="$PATH:$S8_WORK/qemu-build"       # qemu-img from our build if not installed
OVERLAY=$S8_WORK/run/overlay.qcow2
GDB=""; WAIT=""; SNAP=""; HEADLESS=0; CROP=0; FRESH=0; PAYLOAD=1; BOOTARGS=""; MEM=2560M; SMP=4
while [ $# -gt 0 ]; do
  case "$1" in
    --gdb) GDB=1 ;;
    --gdb-wait) GDB=1; WAIT=1 ;;
    --snapshot) SNAP=${2:?--snapshot NAME}; shift ;;
    --headless) HEADLESS=1 ;;
    --crop) CROP=1 ;;
    --fresh) FRESH=1 ;;
    --no-payload) PAYLOAD=0 ;;
    --bootargs) BOOTARGS=${2:?}; shift ;;
    --mem) MEM=${2:?}; shift ;;
    --smp) SMP=${2:?}; shift ;;
    -h|--help) sed -n 2,22p "$0"; exit 0 ;;
    --) shift; break ;;
    *) echo "unknown option $1 (see --help)"; exit 2 ;;
  esac
  shift
done
missing=0
need() { echo "MISSING: $1"; missing=1; }

[ -x "$QEMU" ] || need "QEMU binary $QEMU (build: tools/s8-qemu/qemu/build.sh, Agent B)"
OUT=${OUT:-$S8_WORK/boot-out}
if [ $PAYLOAD = 1 ]; then
  if [ -x "$HERE/boot/make-payload.sh" ]; then
    echo "== payload: boot/make-payload.sh"
    if ! bash "$HERE/boot/make-payload.sh" $BOOTARGS > "$S8_WORK/run-payload.log" 2>&1; then
      tail -20 "$S8_WORK/run-payload.log"; need "payload build (see $S8_WORK/run-payload.log)"
    fi
  else
    need "boot/make-payload.sh (Agent D)"
  fi
fi
[ -f "$OUT/qemu-args.txt" ] || need "$OUT/qemu-args.txt (written by boot/make-payload.sh)"
if [ ! -e "$BASE" ]; then
  need "base disk image $BASE (docs/platform.md 4.2: python3 -I tools/s8-qemu/docs/helpers/make_gpt.py <root.img> $BASE; or set S8_DISK_BASE)"
fi
if [ $missing = 1 ]; then
  echo "Cannot start; fix the MISSING items above."; exit 1
fi

mkdir -p "$S8_WORK/run"
[ $FRESH = 1 ] && rm -f "$OVERLAY"
if [ ! -e "$OVERLAY" ]; then
  # copy-on-write overlay: the base image is never modified
  qemu-img create -q -f qcow2 -b "$BASE" -F raw "$OVERLAY" || { echo "overlay creation failed"; exit 1; }
fi
TS=$(date +%Y%m%d-%H%M%S)
LOGDIR=$S8_WORK/run/$TS
mkdir -p "$LOGDIR"
ln -sfn "$LOGDIR" "$S8_WORK/run/latest"
QMP=$S8_WORK/run/qmp.sock
rm -f "$QMP"

ARGS=(-machine "s8-a5xelte,crop=$([ $CROP = 1 ] && echo on || echo off)" -cpu cortex-a53 -smp "$SMP" -m "$MEM"
  -chardev "stdio,id=con0,signal=off,logfile=$LOGDIR/serial.log" -serial chardev:con0 -monitor none
  -qmp "unix:$QMP,server=on,wait=off"
  -drive "file=$OVERLAY,if=none,id=ufs0,format=qcow2" -global exynos-ufs.drive=ufs0
  -D "$LOGDIR/qemu.log" -d guest_errors,unimp)
if [ $HEADLESS = 1 ] || [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
  [ $HEADLESS = 0 ] && echo "note: no DISPLAY (WSLg off?) -> running headless"
  ARGS+=(-display none)
else
  ARGS+=(-display gtk,zoom-to-fit=on)
fi
[ -n "$GDB" ] && ARGS+=(-gdb tcp::1234) && echo "gdb stub on tcp::1234 (tools/s8-qemu/gdb.sh)"
[ -n "$WAIT" ] && ARGS+=(-S) && echo "CPUs halted at reset: connect gdb and 'continue'"
if [ -n "$SNAP" ]; then
  if qemu-img snapshot -l "$OVERLAY" 2>/dev/null | grep -qw "$SNAP"; then
    ARGS+=(-loadvm "$SNAP"); echo "loading snapshot $SNAP"
  else
    echo "note: snapshot '$SNAP' not in $OVERLAY yet (save with: devices/qmp.py hmp 'savevm $SNAP'); booting normally"
  fi
fi
read -r -a PAYARGS < "$OUT/qemu-args.txt"
echo "== logs: $LOGDIR   QMP: $QMP"
echo "== $QEMU ${ARGS[*]} ${PAYARGS[*]} $*"
exec "$QEMU" "${ARGS[@]}" "${PAYARGS[@]}" "$@"
