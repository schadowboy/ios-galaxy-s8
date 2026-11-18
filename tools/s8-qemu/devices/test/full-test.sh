#!/bin/bash
# End-to-end device test: boots fbtest, takes a screendump, injects touch/drag/keys over QMP, prints the guest log.
#   full-test.sh [qemu-binary]
H="$(cd "$(dirname "$0")" && pwd)"
Q=${1:-/root/work/s8-qemu/qemu-e-scratch/build/qemu-system-aarch64}
bash "$H/../apply-devices.sh" /root/work/s8-qemu/qemu-e-scratch >/dev/null 2>&1
( cd /root/work/s8-qemu/qemu-e-scratch/build && ninja qemu-system-aarch64 >/tmp/n.log 2>&1 || { tail /tmp/n.log; exit 1; } )
OUT=/root/work/s8-qemu/e-test
timeout 30 bash "$H/build-run.sh" "$Q" > $OUT/serial.txt 2>&1 &
sleep 6
QMP="python3 -I $H/../qmp.py -s /tmp/s8e-qmp.sock"
$QMP screendump $OUT/shot.ppm
$QMP touch 16384 16384 200
sleep 0.3
$QMP drag 8000 8000 20000 24000
sleep 0.3
$QMP key pgup 200
$QMP key p 200
$QMP key h 200
sleep 0.5
$QMP hmp "info qtree" | grep -c s8
wait
cat $OUT/serial.txt
ls -l $OUT/shot.ppm
head -c 20 $OUT/shot.ppm | head -2
