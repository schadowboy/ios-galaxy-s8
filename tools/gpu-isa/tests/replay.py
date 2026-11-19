import json, sys, os, re
# replay.py <capdir> <session> <region> [startoff endoff]: state of region as of the end of <session>, printed as dwords
d = sys.argv[1]
sess = int(sys.argv[2]); reg = sys.argv[3]
idx = {}
for l in open(os.path.join(d, 'index.txt')):
    m = re.match(r'session\s+(\d+) (\S+)\s+type (\d+) va 0x([0-9a-f]+) len 0x([0-9a-f]+)', l)
    if m: idx[m.group(2)] = (int(m.group(4), 16), int(m.group(5), 16))
va, ln = idx[reg]
buf = bytearray(ln)
for s in range(1, sess + 1):
    p = os.path.join(d, 'session_%d.json' % s)
    if not os.path.exists(p): continue
    j = json.load(open(p))
    if reg in j:
        for o, h in j[reg].items():
            buf[int(o, 16):int(o, 16) + 64] = bytes.fromhex(h)
if len(sys.argv) > 6:   # replay.py <cap> <session> <region> <a> <b> <out.bin>: also write the replayed state
    open(sys.argv[6], 'wb').write(bytes(buf))
a = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0
b = int(sys.argv[5], 16) if len(sys.argv) > 5 else ln
print('%s va 0x%x len 0x%x session %d' % (reg, va, ln, sess))
for o in range(a, b, 16):
    w = [int.from_bytes(buf[o + 4 * k:o + 4 * k + 4], 'little') for k in range(4)]
    if not any(w): continue
    print('%05x: %08x %08x %08x %08x' % (o, *w))
