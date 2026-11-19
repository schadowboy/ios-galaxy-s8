import sys, struct
S = sys.argv[1] if len(sys.argv) > 1 else '10'
sys.argv = ['x', 'data/capture17', S]
exec(open('tests/stream_py.py').read().split("if __name__")[0])
print('session', S)
def pushes(ptr):
    base = 0x100000000 | (ptr & ~0x1f)
    out = []
    for i in range(0, 32, 2):
        w0 = rd(base + 4 * i); w1 = rd(base + 4 * i + 4)
        if w0 is None or w1 is None: break
        if (w0 & 0xf) in (3, 7) and (w1 >> 24) == 1:
            out.append((w0, w1, (w1 << 8) | ((w0 >> 24) & 0xfe)))
        else: break
    return out
for base, p, w, h in passes():
    g = lambda o: struct.unpack_from('<I', shm, base + o)[0]
    dims = (g(0x690), g(0x694)) if base + 0x698 <= len(shm) else (0, 0)
    print('block %x dims %dx%d' % (base, dims[0], dims[1]))
    for o in (0x2b0, 0x2d0):
        for (w0, w1, va) in pushes(g(o)):
            print('    push %08x %08x -> %x %s: %s' % (w0, w1, va, name(va), words(va, 12)))
