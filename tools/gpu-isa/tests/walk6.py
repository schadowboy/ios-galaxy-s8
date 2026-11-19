import sys, struct
S = sys.argv[1] if len(sys.argv) > 1 else '10'
sys.argv = ['x', 'data/capture17', S]
exec(open('tests/stream_py.py').read().split("if __name__")[0])
# texture/sampler pushes in all fragment slots of res_17 (every 0x20)
seen = set()
d = bufs['res_17']
for o in range(0, len(d) - 8, 4):
    w0, w1 = struct.unpack_from('<II', d, o)
    if (w0 & 0xff) in (0x17, 0x13) and (w1 >> 24) == 1:
        va = (w1 << 8) | ((w0 >> 24) & 0xfe)
        key = (w0 & 0xff, va)
        if key in seen: continue
        seen.add(key)
        print('res_17+%04x %08x %08x -> %x %s : %s' % (o, w0, w1, va, name(va), words(va, 8)))
