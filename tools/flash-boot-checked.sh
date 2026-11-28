#!/bin/bash
# flash-boot-checked.sh <image name without .img>: flash images/<name>.img to BOOT only when the file exists, is not empty and is not the
# image of a failed build; then reboot is up to the caller (and only after "FLASH VERIFIED"). Wraps /c/github/ios-s8/tools/flash-boot.sh.
# Why: 2026-11-28 a failed kernel build led to a flash attempt with no image (nothing was written: dd could not read its input, BOOT kept the
# previous image) and a reboot of the phone anyway.
N=${1:?image name}
IMG="/c/github/ios-s8/images/$N.img"
if [ ! -s "$IMG" ]; then
  echo "FLASH REFUSED: $IMG is missing or empty"
  exit 1
fi
OUT=$(bash /c/github/ios-s8/tools/flash-boot.sh "$N" 2>&1)
echo "$OUT" | tail -3
case "$OUT" in
  *"FLASH VERIFIED"*) exit 0 ;;
  *) echo "FLASH NOT VERIFIED: do not reboot into this"; exit 2 ;;
esac
