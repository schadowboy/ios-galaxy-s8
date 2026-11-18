#!/usr/bin/env python3
"""ppm2png.py IN.ppm OUT.png [DOWNSCALE=1]  (stdlib only; for QMP screendump files)"""
import struct, sys, zlib

d = open(sys.argv[1], 'rb').read()
parts = d.split(b'\n', 3)
w, h = map(int, parts[1].split())
pix = parts[3]
n = int(sys.argv[3]) if len(sys.argv) > 3 else 1
rows = []
for y in range(0, h - n + 1, n):
    r = bytearray()
    for x in range(0, w - n + 1, n):
        o = (y * w + x) * 3
        r += pix[o:o + 3]
    rows.append(b'\0' + bytes(r))
raw = b''.join(rows)


def chunk(t, c):
    return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c))


png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w // n, h // n, 8, 2, 0, 0, 0)) \
    + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
open(sys.argv[2], 'wb').write(png)
