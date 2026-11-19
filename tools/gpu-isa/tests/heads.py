import sys, glob, os, struct, collections
# heads.py <capdir>...: distinct pass header pairs {w0 with top byte 0xf8-0xff or 0xc?/1f attachments, w1} seen in all final_res_*.bin, with counts
for cap in sys.argv[1:]:
    c = collections.Counter()
    a = collections.Counter()
    for f in glob.glob(os.path.join(cap, 'final_res_*.bin')):
        d = open(f, 'rb').read()
        n = len(d) // 4
        w = struct.unpack('<%dI' % n, d[:n * 4])
        for i in range(n - 6):
            if (w[i] >> 24) >= 0xf8 and (w[i + 2] >> 28) in (4, 3, 8, 0xb) and (w[i + 1] & 0xffff) != 0 and (w[i] & 0xff) in (0x0a, 0xc2, 0x82, 0x4a, 0x2c, 0x0d):
                c[(w[i], w[i + 1], w[i + 3] if w[i + 3] in (0, 0x8007c000, 0xbfffc000) else 1)] += 1
            if w[i + 3] == 0x200 and 0x10000000 <= w[i + 2] < 0x10200000:
                a[(w[i], w[i + 1])] += 1
    print('==', cap)
    for k, v in sorted(c.items()):
        print('head %08x %08x B=%x x%d' % (k[0], k[1], k[2], v))
    for k, v in sorted(a.items()):
        print('att  %08x %08x x%d' % (k[0], k[1], v))
