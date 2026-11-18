#!/bin/bash
# Smoke-test run.sh degradation and read-log.sh against a synthetic GPT-less image with a SWEETLOG ring.
H="$(cd "$(dirname "$0")/../.." && pwd)"
T=/root/work/s8-qemu/e-test
mkdir -p $T
echo "--- run.sh (no base image / no QEMU expected to be reported)"
S8_DISK_BASE=/nonexistent bash $H/run.sh --no-payload --headless 2>&1 | tail -5
echo "--- read-log.sh on synthetic image"
python3 -I - <<'PY'
import struct
ring = 4096 * 1024
msg = b"hello from the kernel ring\nsecond line\n"
img = bytearray(8 * 1024 * 1024)
base = len(img) - 1025 * 4096
img[base:base + 32] = struct.pack('<8sIIQQ', b'SWEETLOG', 1, ring, len(msg), 1)
img[base + 4096:base + 4096 + len(msg)] = msg
open('/root/work/s8-qemu/e-test/synth.img', 'wb').write(img)
PY
bash $H/read-log.sh --image $T/synth.img
qemu-img create -q -f qcow2 -b $T/synth.img -F raw $T/synth.qcow2 && bash $H/read-log.sh --image $T/synth.qcow2 | head -2
