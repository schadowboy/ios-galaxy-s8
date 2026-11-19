import sys
# hx.py <file> <offset hex> <len hex>: hex bytes with offsets
d = open(sys.argv[1], 'rb').read()
o = int(sys.argv[2], 16); n = int(sys.argv[3], 16)
for a in range(o, o + n, 16):
    print('%05x: %s' % (a, d[a:a + 16].hex(' ')))
