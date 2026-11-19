import sys, struct
sys.argv = ['x', 'data/capture17', '10']
exec(open('tests/stream_py.py').read().split("if __name__")[0])
for lo, n in [(0xe1640, 100), (0x2f358, 16), (0x2cf58, 16), (0x2ec00, 24), (0xe2940, 50)]:
    va = 0x100000000 | lo
    print(name(va))
    for i in range(0, n, 8):
        print('  %x: %s' % (lo + 4 * i, words(va + 4 * i, 8)))
