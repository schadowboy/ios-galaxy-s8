#!/bin/bash
# Kernel console log of the last s8-qemu run.
#   read-log.sh [--ram] [--image FILE] [--out FILE]
# Default: the SWEETLOG ring in the last 1025 4-KiB blocks of the disk (what tools/sweet-log-pull.sh reads
# from the phone's userdata), straight from the qcow2 overlay (or any raw/qcow2 file via --image), decoded
# with tools/sweet-log.py (the donor decode-sweetlog logic).
#   --ram  read the RAM klog ring instead (persistent_ram "DBGC", phys 0x92180000, 0x80000 bytes) from the
#          RUNNING VM through QMP pmemsave: works before UFS does. Also prints the PL011 serial.log tail.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
S8_WORK=${S8_WORK:-/root/work/s8-qemu}
export PATH="$PATH:$S8_WORK/qemu-build"
IMG=$S8_WORK/run/overlay.qcow2; MODE=disk; OUTF=""
while [ $# -gt 0 ]; do
  case "$1" in
    --ram) MODE=ram ;;
    --image) IMG=${2:?}; shift ;;
    --out) OUTF=${2:?}; shift ;;
    -h|--help) sed -n 2,9p "$0"; exit 0 ;;
    *) echo "unknown option $1"; exit 2 ;;
  esac
  shift
done
emit() { if [ -n "$OUTF" ]; then cat > "$OUTF"; echo "wrote $OUTF" >&2; else cat; fi; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
if [ $MODE = ram ]; then
  if [ ! -S $S8_WORK/run/qmp.sock ]; then
    echo "MISSING: running VM (no $S8_WORK/run/qmp.sock). Serial log of the last run:" >&2
    [ -f $S8_WORK/run/latest/serial.log ] && tail -n 60 $S8_WORK/run/latest/serial.log
    exit 1
  fi
  python3 -I "$HERE/devices/qmp.py" pmemsave 0x92180000 0x80000 "$TMP/klog.bin" || exit 1
  python3 -I - "$TMP/klog.bin" <<'PY' | emit
import struct, sys
d = open(sys.argv[1], 'rb').read()
sig, start, size = struct.unpack_from('<III', d, 0)
cap = len(d) - 12
if sig != 0x43474244:
    sys.exit('no DBGC header at 0x92180000 (kernel has not started its klog yet)')
ring = d[12:]
text = ring[:size] if size < cap else ring[start:] + ring[:start]
sys.stdout.write(text.decode('utf-8', 'replace').replace('\r', ''))
PY
  exit 0
fi
[ -e "$IMG" ] || { echo "MISSING: image $IMG (run run.sh first, or pass --image)"; exit 1; }
command -v qemu-img >/dev/null || { echo "MISSING: qemu-img"; exit 1; }
FMT=$(qemu-img info --output=json "$IMG" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["format"], d["virtual-size"])')
set -- $FMT
fmt=$1; vsize=$2
# The image is a whole UFS LUN with a GPT (4 KiB LBAs, docs/platform.md 3.x): the ring is the last 1025
# blocks of the USERDATA partition. Without a GPT (raw partition image) it is the end of the file.
skip=$((vsize / 4096 - 1025))
if qemu-img dd -f "$fmt" -O raw bs=4096 skip=1 count=9 if="$IMG" of="$TMP/gpt.bin" 2>/dev/null; then
  g=$(python3 -I - "$TMP/gpt.bin" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
if d[:8] == b'EFI PART':
    for o in range(4096, len(d) - 127, 128):
        e = d[o:o + 128]
        if e[56:128].decode('utf-16le', 'ignore').split('\0')[0] == 'USERDATA':
            print(struct.unpack_from('<Q', e, 40)[0] - 1024)
            break
PY
)
  [ -n "$g" ] && skip=$g
fi
skip=${S8_LOG_SKIP_BLOCKS:-$skip}
qemu-img dd -f "$fmt" -O raw bs=4096 skip=$skip count=1025 if="$IMG" of="$TMP/area.bin" 2>"$TMP/dd.err" \
  || { cat "$TMP/dd.err"; echo "qemu-img dd failed"; exit 1; }
python3 -I "$HERE/../sweet-log.py" "$TMP/area.bin" | emit
