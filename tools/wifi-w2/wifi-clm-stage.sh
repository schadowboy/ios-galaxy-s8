#!/bin/bash
# W3b staging: embed Samsung's CLM blob next to the firmware/NVRAM in the WSL loader tree (proprietary, never committed).
# Run once, after tools/wifi-fw-stage.sh (which creates fw/ and the original wifi_fw.S); idempotent.
set -e
S=/mnt/c/github/ios-s8
W=$S/upstream/.claude/worktrees/gpu-stress
R=/root/work/upstream/shim/dreamlte
mkdir -p $R/fw
cp $S/firmware/samsung/wifi/bcmdhd_clm.blob $R/fw/wifi_clm.bin
# the worktree's wifi_fw.S now also exports wifi_clm / wifi_clm_end (build-v1xx.sh does not copy it, so copy it here)
cp $W/shim/dreamlte/wifi_fw.S $R/wifi_fw.S
ls -l $R/fw
grep -n "wifi_clm" $R/wifi_fw.S
