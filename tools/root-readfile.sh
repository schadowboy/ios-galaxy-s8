#!/bin/bash
# Read files out of the phone's current root WITHOUT writing anything back: base image + pulled chunks, repair, mount READ-ONLY, copy files out.
# usage: root-readfile.sh <base-img> <scratch-img> <pulled-dir> <vname> <outdir> <abs path in root>...
set -e
BASE=$1; OUT=$2; PULL=$3; V=$4; DEST=$5; shift 5
MP=/mnt/gi-rw
/root/load-hfsplus.sh >/dev/null 2>&1
cp --sparse=always "$BASE" "$OUT"
for i in $(tr -d '\r' < "$PULL/changed-$V.txt"); do
  dd if="$PULL/chunk-$i.bin" of="$OUT" bs=33554432 seek="$i" conv=notrunc status=none
done
mkdir -p $MP "$DEST"
mountpoint -q $MP && umount $MP
# read-only copy-out: no repair pass (it took about 8 minutes); only when a read-only mount fails is the volume repaired first
if ! mount -t hfsplus -o loop,force,ro "$OUT" $MP 2>/dev/null; then
  fsck.hfsplus -f -y "$OUT" 2>&1 | tr -d '\000' | tail -1 || true
  mount -t hfsplus -o loop,force,ro "$OUT" $MP
fi
[ -z "${FINDCRASH:-}" ] || find $MP/private/var -iname '*.ips' -o -iname '*backboardd*crash*' -o -iname '*.crash' 2>/dev/null | head -20
for p in "$@"; do cp -rv "$MP$p" "$DEST/" || echo "missing $p"; done
umount $MP
rm -f "$OUT"
sync; echo 3 > /proc/sys/vm/drop_caches
