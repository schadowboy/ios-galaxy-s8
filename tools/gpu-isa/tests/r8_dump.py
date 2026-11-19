import sys
# r8_dump.py <res .bin> <texel offset hex> <width> <height> <pitch> <out.ppm>: R8 texture -> grey P6 (then tests/ppm2png.py)
d = open(sys.argv[1], 'rb').read()
o = int(sys.argv[2], 16); w, h, p = int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
rows = [d[o + y * p:o + y * p + w].ljust(w, b'\0') for y in range(h)]
nz = sum(1 for r in rows for b in r if b)
print('nonzero texels', nz, 'of', w * h, 'max', max(max(r) for r in rows))
open(sys.argv[6], 'wb').write(b'P6\n%d %d\n255\n' % (w, h) + b''.join(bytes(v for b in r for v in (b, b, b)) for r in rows))
