#!/usr/bin/env python3
# replay_dump.py <capdir> <session> <outdir>: write the state of every dumped region as of the end of <session> (replaying the session_N.json deltas,
# like tests/replay.py) as <outdir>/<name>.bin plus <outdir>/regions.txt ("<name> <va hex> <len hex>"). tests/stream_decode.c reads such a directory.
# Lines that became all-zero are not in the capture (the dump skips zero lines), so a region can show stale data where userland zeroed it.
import sys, os, re, json
cap, sess, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
os.makedirs(out, exist_ok=True)
regs = {}
for l in open(os.path.join(cap, 'index.txt')):
    m = re.match(r'session\s+(\d+) (\S+)\s+type (\d+) va 0x([0-9a-f]+) len 0x([0-9a-f]+)', l)
    if m: regs[m.group(2)] = (int(m.group(4), 16), int(m.group(5), 16))
bufs = {n: bytearray(ln) for n, (va, ln) in regs.items()}
for s in range(1, sess + 1):
    p = os.path.join(cap, 'session_%d.json' % s)
    if not os.path.exists(p): continue
    for n, lines in json.load(open(p)).items():
        for o, h in lines.items():
            bufs[n][int(o, 16):int(o, 16) + 64] = bytes.fromhex(h)
with open(os.path.join(out, 'regions.txt'), 'w') as f:
    for n, (va, ln) in sorted(regs.items()):
        open(os.path.join(out, n + '.bin'), 'wb').write(bytes(bufs[n]))
        f.write('%s %x %x\n' % (n, va, ln))
