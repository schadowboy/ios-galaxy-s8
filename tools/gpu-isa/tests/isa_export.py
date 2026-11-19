import json, os, re, struct, sys
# isa_export.py <capture dir> <out file> [first] [last]: raw 256 bytes of the program heap at res_3 + 0xc80 as of each session, as [u32 id][u32 len][bytes]
d, out = sys.argv[1], sys.argv[2]
first = int(sys.argv[3]) if len(sys.argv) > 3 else 1
last = int(sys.argv[4]) if len(sys.argv) > 4 else 99
mem = {}
f = open(out, 'wb')
nums = sorted(int(re.match(r'session_(\d+)\.json', x).group(1)) for x in os.listdir(d) if re.match(r'session_\d+\.json', x))
for n in nums:
    s = json.load(open(os.path.join(d, 'session_%d.json' % n)))
    for o, hx in s.get('res_3', {}).items():
        mem[int(o, 16)] = bytes.fromhex(hx)
    if first <= n <= last:
        raw = bytearray()
        for a in range(0xc80, 0xc80 + 256):
            line = mem.get(a & ~63)
            raw.append(line[a & 63] if line else 0)
        f.write(struct.pack('<II', n, len(raw)) + bytes(raw))
f.close()
print('exported', out)
