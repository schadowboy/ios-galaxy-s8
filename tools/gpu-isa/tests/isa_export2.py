import json, os, re, sys
# isa_export2.py <capture dir> <first> <last>: for each session, find a fragment program that ends with `f2 ff 08 01` (st_tile + end) in the res_3 lines
# changed by that session and print the 192 bytes before the end (merged program heap)
d = sys.argv[1]
first, last = int(sys.argv[2]), int(sys.argv[3])
mem = {}
nums = sorted(int(re.match(r'session_(\d+)\.json', x).group(1)) for x in os.listdir(d) if re.match(r'session_\d+\.json', x))
for n in nums:
    s = json.load(open(os.path.join(d, 'session_%d.json' % n)))
    ch = s.get('res_3', {})
    for o, hx in ch.items():
        mem[int(o, 16)] = bytes.fromhex(hx)
    if not (first <= n <= last):
        continue
    for o in sorted(ch, key=lambda x: int(x, 16)):
        base = int(o, 16)
        for a in range(base - 192, base + 64):
            got = bytearray()
            for k in range(a, a + 12):
                line = mem.get(k & ~63)
                got.append(line[k & 63] if line else 0)
            if bytes(got[:10]) == bytes.fromhex('090200000000f2ff0801'):
                raw = bytearray()
                for k in range(a - 192, a + 12):
                    line = mem.get(k & ~63)
                    raw.append(line[k & 63] if line else 0)
                print('session', n, 'end at 0x%x' % a, raw.hex())
