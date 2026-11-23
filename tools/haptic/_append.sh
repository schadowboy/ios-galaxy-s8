#!/bin/bash
# Insert tools/haptic/haptic.c into shim/dreamlte/smp_probe.c between its own BEGIN/END markers (the build scripts copy
# only smp_probe.c + loader.c). Idempotent. The block is placed BEFORE the wifi_w2.c block when that exists, because
# tools/wifi-w2/_append.sh deletes everything from its BEGIN marker to the end of the file.
W=/mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress
F=$W/shim/dreamlte/smp_probe.c
T=/tmp/haptic_new_smp.c
if grep -q "BEGIN haptic.c" "$F"; then
  sed -i '/^\/\* ==== BEGIN haptic.c/,/^\/\* ==== END haptic.c/d' "$F"
fi
{
  echo "/* ==== BEGIN haptic.c (copy of tools/haptic/haptic.c; edit THERE and re-run tools/haptic/_append.sh) ==== */"
  cat $W/tools/haptic/haptic.c
  echo "/* ==== END haptic.c ==== */"
} > /tmp/haptic_block.c
if grep -q "BEGIN wifi_w2.c" "$F"; then
  N=$(grep -n "BEGIN wifi_w2.c" "$F" | head -1 | cut -d: -f1)
  { head -n $((N-1)) "$F"; cat /tmp/haptic_block.c; tail -n +$N "$F"; } > $T
  cat $T > "$F"
else
  cat /tmp/haptic_block.c >> "$F"
fi
echo "haptic blocks: $(grep -c 'BEGIN haptic.c' "$F")  wifi blocks: $(grep -c 'BEGIN wifi_w2.c' "$F")"
