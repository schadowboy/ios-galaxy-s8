import sys, struct
sys.argv = ['x', 'data/capture17']
exec(open('tests/stream_py.py').read().split("if __name__")[0])
starts = []
for o in range(0, len(shm) - 0x40, 4):
    a, b, c, d = struct.unpack_from('<IIII', shm, o)
    if (a, b, c, d) == (0x818, 0x7e8, 0x30, 1):
        p = struct.unpack_from('<Q', shm, o + 16)[0]
        starts.append((o - 0x28, p))
print([('%x' % o, '%x' % p) for o, p in starts])
for o, p in starts:
    print('=== block %x stream %x' % (o, p))
    q = p
    n = 0
    while n < 400:
        w = rd(q)
        if w is None: print('  <unmapped %x>' % q); break
        if w == 0x80000001:
            t = rd(q + 4); print('  %x LINK -> %x' % (q & 0xffff, t)); q = 0x100000000 | t; n += 1; continue
        if w == 0xc0000000:
            print('  %x END' % (q & 0xffff)); break
        if (w & 0xffff00ff) == 1:
            c = (w >> 8) & 0xff; t = rd(q + 4)
            body = ' '.join(H(rd(0x100000000 | t + 4 * i)) for i in range(c))
            print('  %x LOAD%d %08x %s: %s' % (q & 0xffff, c, t, name(0x100000000 | t), body)); q += 8; n += 1; continue
        print('  %x raw %08x' % (q & 0xffff, w)); q += 4; n += 1
