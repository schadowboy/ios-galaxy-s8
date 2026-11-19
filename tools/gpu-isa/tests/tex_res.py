#!/usr/bin/env python3
# tex_res.py <frag_forms.txt> <log> [<log2>...]: sampled textures (TEX lines of tests/frag_forms.c) -> resource id from the kernel log, IOSurface id, descriptor words.
# Resource lines: "S8AGX: resource N: size 0x.. flags 0x.. [(...)] gpu va 0x.. cpu 0x.."  and  "S8AGX: resource N is IOSurface S (gpu va 0x..)".
# The log mixes several clients (backboardd, SpringBoard): every covering candidate is listed, the last one in the log wins the "pick" column.
import re, sys
tex = []
for l in open(sys.argv[1]):
    m = re.match(r'TEX ((?:[0-9a-f]{8} ){6}) va ([0-9a-f]+) form (\d) dest (\d+) uses (\d+)', l)
    if m:
        w = [int(x, 16) for x in m.group(1).split()]
        tex.append((w, int(m.group(2), 16), int(m.group(3)), int(m.group(4)), int(m.group(5))))
res = []      # (id, va, size, flags, surf)
surf = {}
for lp in sys.argv[2:]:
    for l in open(lp, errors='replace'):
        m = re.search(r'S8AGX: resource (\d+): size 0x([0-9a-f]+) flags 0x([0-9a-f]+).*gpu va 0x([0-9a-f]+)', l)
        if m: res.append([int(m.group(1)), int(m.group(4), 16), int(m.group(2), 16), int(m.group(3), 16), None]); continue
        m = re.search(r'S8AGX: resource (\d+) is IOSurface (\d+) \(gpu va 0x([0-9a-f]+)\)', l)
        if m: surf[(int(m.group(1)), int(m.group(3), 16))] = int(m.group(2))
for r in res:
    r[4] = surf.get((r[0], r[1]))
seen = {}
for w, va, form, dest, uses in tex:
    if form == 0 or va == 0: continue
    seen.setdefault(va, []).append((w, dest, uses))
print('texture VA      resource (id flags size surface)           distinct records  descriptor words (w0 w1 w2 w3 w4 w5)')
for va in sorted(seen):
    cands = [r for r in res if r[1] <= va < r[1] + r[2] and r[1] >= 0x100900000]
    pick = cands[-1] if cands else None
    if pick: desc = 'res %d flags 0x%x size 0x%x%s' % (pick[0], pick[3], pick[2], (' IOSurface %d' % pick[4]) if pick[4] else '')
    else: desc = 'no resource line covers it (id < 94 or not logged)'
    for w, dest, uses in seen[va][:3]:
        print('%-14x  %-48s %s  dest %d uses %d' % (va, desc, ' '.join('%08x' % x for x in w), dest, uses))
        desc = ''
        va_s = ''
print()
print('records without a decodable address (sampler / other 0x17 pushes):')
for w, va, form, dest, uses in tex:
    if form == 0 or va == 0: print('  ' + ' '.join('%08x' % x for x in w), 'dest', dest, 'uses', uses)
