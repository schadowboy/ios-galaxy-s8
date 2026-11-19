import json, sys, os, re, struct
# scanatt.py <capdir> <session>: replay all regions to the end of <session> and list attachment records
#   { w0 (low16 = format code), w1, VA>>4, 0x00000200 }  (the test layout 1fe3050a 000007e0 <VA>>4 00000200 and its compositor relatives)
d = sys.argv[1]
sess = int(sys.argv[2])
idx = {}
for l in open(os.path.join(d, 'index.txt')):
    m = re.match(r'session\s+(\d+) (\S+)\s+type (\d+) va 0x([0-9a-f]+) len 0x([0-9a-f]+)', l)
    if m: idx[m.group(2)] = (int(m.group(4), 16), int(m.group(5), 16))
bufs = {k: bytearray(v[1]) for k, v in idx.items()}
for s in range(1, sess + 1):
    p = os.path.join(d, 'session_%d.json' % s)
    if not os.path.exists(p): continue
    for reg, lines in json.load(open(p)).items():
        for o, h in lines.items():
            bufs[reg][int(o, 16):int(o, 16) + 64] = bytes.fromhex(h)
for reg, b in sorted(bufs.items()):
    if reg.startswith('shmem'): continue
    n = len(b) // 4
    w = struct.unpack('<%dI' % n, bytes(b[:n * 4]))
    for i in range(n - 4):
        if w[i + 3] == 0x200 and w[i + 2] >= 0x10000000 and w[i + 2] < 0x10200000 and w[i + 1] != 0 and w[i] != 0:
            print('%s +%05x: %08x %08x VA 0x%x  %08x %08x' % (reg, i * 4, w[i], w[i + 1], w[i + 2] << 4, w[i + 4], w[i + 5]))
