import sys, collections
# vprol.py <region .bin files>: histogram of the vertex-program prologue (bytes from `c2 02 02 00` for 40 bytes)
cnt = collections.Counter()
for fn in sys.argv[1:]:
    d = open(fn, 'rb').read()
    i = 0
    while True:
        i = d.find(bytes([0xc2, 0x02, 0x02, 0x00]), i)
        if i < 0: break
        cnt[d[i:i + 28].hex(' ')] += 1
        i += 4
for k, v in cnt.most_common(25):
    print(v, k)
