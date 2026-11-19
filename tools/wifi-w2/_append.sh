#!/bin/bash
# append tools/wifi-w2/wifi_w2.c to shim/dreamlte/smp_probe.c (build-v141.sh copies only smp_probe.c + loader.c into the build tree)
# idempotent: replaces a previous copy between the markers.
W=/mnt/c/github/ios-s8/upstream/.claude/worktrees/gpu-stress
F=$W/shim/dreamlte/smp_probe.c
if grep -q "BEGIN wifi_w2.c" "$F"; then
  sed -i '/^\/\* ==== BEGIN wifi_w2.c/,$d' "$F"
fi
{
  echo "/* ==== BEGIN wifi_w2.c (copy of tools/wifi-w2/wifi_w2.c + wifi_w3.c; edit THERE and re-run tools/wifi-w2/_append.sh) ==== */"
  cat $W/tools/wifi-w2/wifi_w2.c $W/tools/wifi-w2/wifi_w3.c
  echo "/* ==== END wifi_w2.c ==== */"
} >> "$F"
grep -c "BEGIN wifi_w2.c" "$F"
