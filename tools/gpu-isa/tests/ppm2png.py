#!/usr/bin/env python3
# ppm2png.py <in.ppm> <out.png>: binary P6 -> PNG (zlib only, no PIL needed)
import sys, zlib, struct
d = open(sys.argv[1], 'rb').read()
# header: P6\n W H\n255\n
parts = d.split(b'\n', 3)
w, h = map(int, parts[1].split())
pix = parts[3]
raw = b''.join(b'\x00' + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))
def chunk(t, c): return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
open(sys.argv[2], 'wb').write(png)
