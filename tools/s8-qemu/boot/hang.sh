#!/bin/bash
# Run the scratch virt boot for N seconds (default 20), then stop it through the monitor and dump registers + the frame chain.
# usage: hang.sh [secs]
OUT=${OUT:-/root/work/s8-qemu/boot-out-virt}
rm -f /tmp/s8mon
qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a53 -smp 1 -m 4G -display none -serial file:$OUT/hang-serial.log -no-reboot \
  -monitor unix:/tmp/s8mon,server,nowait -d guest_errors,unimp -D $OUT/hang-qemu.log \
  -device loader,file=$OUT/payload.bin,addr=0x44000000,force-raw=on \
  -device loader,file=$OUT/loader_qemu.bin,addr=0x40200000,force-raw=on \
  -device loader,addr=0x40200000,cpu-num=0 > /dev/null 2>&1 < /dev/null &
sleep ${1:-20}
python3 -I - <<'EOF'
import socket, time
s = socket.socket(socket.AF_UNIX); s.connect('/tmp/s8mon'); s.settimeout(2)
def cmd(c):
    s.sendall((c + '\n').encode()); time.sleep(0.5)
    try: return s.recv(65536).decode(errors='replace')
    except Exception: return ''
cmd('stop')
print(cmd('info registers'))
r = cmd('x /6i $pc')
print(r)
EOF
pkill -f "qemu-system-aarch64.*s8mon" || true
