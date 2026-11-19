#!/usr/bin/env python3
# crop.py <in.png|ppm> <x> <y> <w> <h> <out.png> [scale]: crop (and integer-upscale) a region of a frame; PPM input is read directly, PNG via zlib (8-bit RGB/RGBA non-interlaced).
import sys, zlib, struct
src, x0, y0, w, h, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), sys.argv[6]
sc = int(sys.argv[7]) if len(sys.argv) > 7 else 1
d = open(src, 'rb').read()
if d[:2] == b'P6':
    parts = d.split(b'\n', 3)
    W, H = map(int, parts[1].split()); px = parts[3]; bpp = 3
else:
    p = 8; idat = b''
    while p < len(d):
        n, t = struct.unpack('>I4s', d[p:p + 8]); b = d[p + 8:p + 8 + n]
        if t == b'IHDR': W, H, bd, ct = struct.unpack('>IIBB', b[:10])
        if t == b'IDAT': idat += b
        p += 12 + n
    bpp = 4 if ct == 6 else 3
    raw = zlib.decompress(idat); stride = W * bpp; px = bytearray(); prev = bytearray(stride)
    for y in range(H):
        f = raw[y * (stride + 1)]; row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = row[i - bpp] if i >= bpp else 0; b = prev[i]; c = prev[i - bpp] if i >= bpp else 0
            if f == 1: row[i] = (row[i] + a) & 255
            elif f == 2: row[i] = (row[i] + b) & 255
            elif f == 3: row[i] = (row[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                pp = a + b - c; pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                row[i] = (row[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        px += row; prev = row
    px = bytes(px)
rows = bytearray()
for y in range(y0, min(H, y0 + h)):
    line = bytearray()
    for x in range(x0, min(W, x0 + w)):
        o = (y * W + x) * bpp
        line += px[o:o + 3] * sc
    for _ in range(sc): rows += b'\0' + line
OW, OH = min(W, x0 + w) - x0, min(H, y0 + h) - y0
def ch(t, b): c = struct.pack('>I', len(b)) + t + b; return c + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', OW * sc, OH * sc, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(bytes(rows))) + ch(b'IEND', b''))
