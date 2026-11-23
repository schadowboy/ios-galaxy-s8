#!/bin/bash
# Install the S8 hook dylib(s) into the PHONE'S CURRENT root (same safe procedure as gl-install3.sh: base image + chunks pulled from the phone).
# usage: hook-install.sh <base-img> <out-img> <pulled-dir> <vnew> <dylib> [<dylib>...]
# Each dylib goes to /usr/local/lib/<basename> (root:wheel 755). Removable test scaffolding: nothing of Apple's is touched.
set -e
BASE=$1; OUT=$2; PULL=$3; V=$4; shift 4
MP=/mnt/gi-rw
WIN=/mnt/c/github/ios-s8
for f in "$BASE" "$PULL/changed-$V.txt" "$PULL/chunksha-$V.txt" "$@"; do
  [ -f "$f" ] || { echo "missing $f"; exit 1; }
done
/root/load-hfsplus.sh >/dev/null 2>&1
cp --sparse=always "$BASE" "$OUT"
for i in $(tr -d '\r' < "$PULL/changed-$V.txt"); do
  dd if="$PULL/chunk-$i.bin" of="$OUT" bs=33554432 seek="$i" conv=notrunc status=none
done
echo "assembled the phone's current root: $(wc -l < "$PULL/changed-$V.txt") chunks applied"
mkdir -p $MP
mountpoint -q $MP && umount $MP
fsck.hfsplus -f -y "$OUT" 2>&1 | tr -d '\000' | tail -3 || true
mount -t hfsplus -o loop,force,rw "$OUT" $MP
grep -q " $MP .*[ ,(]rw[ ,)]" /proc/mounts || { echo "STILL READ-ONLY after repair"; exit 1; }
mkdir -p $MP/usr/local/lib
for d in "$@"; do
  install -o 0 -g 0 -m 755 "$d" $MP/usr/local/lib/$(basename "$d")
done
if [ -n "${OSMESA:-}" ]; then      # optional: a new Mesa into the directory libs8gl searches
  mkdir -p $MP/usr/local/lib/s8mesa
  install -o 0 -g 0 -m 755 "$OSMESA" $MP/usr/local/lib/s8mesa/libOSMesa.8.dylib
fi
ls -la $MP/usr/local/lib/ $MP/usr/local/lib/s8mesa/
sync
umount $MP
fsck.hfsplus -f -y "$OUT" 2>&1 | tr -d '\000' | tail -2 || true
python3 /root/work/upstream/tools/dreamlte/chunk-manifest.py "$OUT" /root/work/ios16-root-$V.json
python3 $WIN/tools/diff-phone.py /root/work/ios16-root-$V.json "$PULL/chunksha-$V.txt" $WIN/images/ios16-root-$V.changed.txt
cp /root/work/ios16-root-$V.json $WIN/images/ios16-root-$V.json
for d in "$@"; do sha256sum "$d"; done
