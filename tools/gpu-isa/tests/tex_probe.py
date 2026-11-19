#!/usr/bin/env python3
# tex_probe.py <capture> <session> <va hex> <width> <height> <bpp> <layout> <out.ppm> [pitch_bytes]
# Reads the region snapshot (tests/replay_dump.py output in /tmp/s8s/<capture>/s<session>), cuts the texels at <va> and writes a PPM.
#   layout: lin | morton (y in low bit) | tile<N> (N x N row-major tiles, linear inside, tiles row-major) | mtile<N> (N x N tiles, Morton inside)
#   bpp: 1 (gray), 4 (BGRA8 shown as RGB), 5 = RGBA8 byte order, 2 (16 bit gray hi byte)
import sys, os, subprocess
cap, sess, va, W, H, bpp, lay, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3], 16), int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6]), sys.argv[7], sys.argv[8]
pitch = int(sys.argv[9]) if len(sys.argv) > 9 else 0
base = '/tmp/s8s/%s/s%d' % (cap, sess)
here = os.path.dirname(os.path.abspath(__file__))
if not os.path.isdir(base):
    subprocess.check_call(['python3', '-I', os.path.join(here, 'replay_dump.py'), os.path.join(here, '..', 'data', cap), str(sess), base])
regs = []
for l in open(base + '/regions.txt'):
    n, a, ln = l.split()
    regs.append((n, int(a, 16), int(ln, 16)))
def mem(a, n):
    for nm, ra, ln in regs:
        if ra <= a < ra + ln:
            d = open('%s/%s.bin' % (base, nm), 'rb').read()
            return d[a - ra:a - ra + n]
    return b''
bw = bpp if bpp != 5 else 4
def morton(x, y, n):
    i = 0
    for b in range(n):
        i |= ((y >> b) & 1) << (2 * b)
        i |= ((x >> b) & 1) << (2 * b + 1)
    return i
def off(x, y):
    if lay == 'lin':
        return y * (pitch or W * bw) + x * bw
    if lay == 'morton':
        return morton(x, y, 16) * bw
    if lay.startswith('tile'):
        n = int(lay[4:]); tw = (W + n - 1) // n
        return (((y // n) * tw + (x // n)) * n * n + (y % n) * n + (x % n)) * bw
    if lay.startswith('mtile'):
        n = int(lay[5:]); tw = (W + n - 1) // n; k = n.bit_length() - 1
        return (((y // n) * tw + (x // n)) * n * n + morton(x % n, y % n, k)) * bw
size = (H + 64) * (pitch or W * bw) + 65536
d = mem(va, min(size, 0x2000000))
print('have', len(d), 'bytes at', hex(va))
rows = []
for y in range(H):
    r = bytearray()
    for x in range(W):
        o = off(x, y)
        p = d[o:o + bw]
        if len(p) < bw: p = bytes(bw)
        if bpp == 1: r += bytes([p[0]] * 3)
        elif bpp == 2: r += bytes([p[1]] * 3)
        elif bpp == 4: r += bytes([p[2], p[1], p[0]])
        else: r += bytes([p[0], p[1], p[2]])
    rows.append(bytes(r))
open(out, 'wb').write(b'P6\n%d %d\n255\n' % (W, H) + b''.join(rows))
