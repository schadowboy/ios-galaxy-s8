import sys
# hh_peek.py <snapshot dir> <va hex> <before> <after>: hex dump of program memory around a VA (regions.txt: name va len; <name>.bin)
d, va, b, a = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 0), int(sys.argv[4], 0)
for line in open(d + '/regions.txt'):
    p = line.split()
    if len(p) < 3: continue
    r0, ln = int(p[1], 16), int(p[2], 16)
    if r0 <= va < r0 + ln:
        data = open('%s/%s.bin' % (d, p[0]), 'rb').read()
        s = max(va - b, r0) - r0
        e = min(va + a, r0 + ln) - r0
        base = r0 + s
        for i in range(s, e, 16):
            print('%x: %s' % (r0 + i, ' '.join('%02x' % x for x in data[i:min(i + 16, e)])) + ('   <-- entry' if r0 + i <= va < r0 + i + 16 else ''))
        break
