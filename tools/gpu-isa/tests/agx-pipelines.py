import json, os, re, sys, struct, collections

# agx-pipelines.py <capture dir>: every shader-state record {xxxx001f|xxxx051f.., low VA, 0x108} seen in the changed lines, counted per (header, pointer)
d = sys.argv[1]
nums = sorted(int(re.match(r'session_(\d+)\.json', f).group(1)) for f in os.listdir(d) if re.match(r'session_\d+\.json', f))
cnt = collections.Counter()
first = {}
for n in nums:
    s = json.load(open(os.path.join(d, 'session_%d.json' % n)))
    for reg, lines in s.items():
        for off, hx in lines.items():
            b = bytes.fromhex(hx)
            dw = struct.unpack('<16I', b)
            for i in range(0, 14):
                if (dw[i] & 0xff) == 0x1f and dw[i + 2] == 0x108 and 0x10000 <= dw[i + 1] < 0x1000000:
                    key = (dw[i], dw[i + 1] & ~1)
                    cnt[key] += 1
                    first.setdefault(key, (n, reg, int(off, 16) + i * 4))
for k, c in cnt.most_common():
    print('hdr %08x ptr %08x  seen %4d  first session %d %s+%x' % (k[0], k[1], c, *first[k]))
print(len(cnt), 'distinct (header, pointer) pairs')
