import sys, struct
sys.argv = ['x', 'data/capture17']
exec(open('tests/stream_py.py').read().split("if __name__")[0])
def words(va, n): return ' '.join(H(rd(va + 4 * i)) for i in range(n))
for p in [0x190304, 0x190345, 0x190f22, 0x1911e2, 0x190b44, 0x190b04, 0x191184, 0x190a84, 0x190744, 0x190944, 0x190804]:
    b = 0x100000000 | (p & ~0x1f)
    print('frag ptr %x:' % p, words(b, 16))
for p in [0x180184, 0x180204, 0x1807c4, 0x180804, 0x180784, 0x180584, 0x180284]:
    b = 0x100000000 | (p & ~0x3f)
    print('vert ptr %x:' % p, words(b, 16))
for va in [0x1006 * 0 + 0x10061ea80, 0x1000e1e80, 0x100617a00, 0x1000e2740, 0x100618f80]:
    print('X', name(va), words(va, 12))
