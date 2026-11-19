import sys, struct
S = sys.argv[1] if len(sys.argv) > 1 else '10'
sys.argv = ['x', 'data/capture17', S]
exec(open('tests/stream_py.py').read().split("if __name__")[0])
print('session', S)
for base, p, w, h in passes():
    b = base
    g = lambda o: struct.unpack_from('<I', shm, b + o)[0]
    print('block %x stream %x  f1 %08x slotA %08x slotB %08x res18ptr %08x %08x ptr2 %08x' % (base, p, g(0x2a8), g(0x2b0), g(0x2d0), g(0x2d8), g(0x2dc), g(0x2e0)))
    for o in (0x2b0, 0x2d0):
        ptr = g(o)
        if ptr:
            va = 0x100000000 | (ptr & ~0x1f)
            print('   slot %08x (%s): %s' % (ptr, name(va), words(va, 16)))
