import sys, struct
sys.argv = ['x', 'data/capture17', sys.argv[1] if len(sys.argv) > 1 else '99']
exec(open('tests/stream_py.py').read().split("if __name__")[0])
d = bufs['res_11']
n = len(d) // 4
w = struct.unpack('<%dI' % n, bytes(d))
for i in range(n):
    if (w[i] >> 24) in (0x6e, 0x7e) and (w[i] >> 16 & 0xff) in (0x40, 0x42, 0x44, 0x46):
        print('%04x: pre %s | %s | post %s' % (i * 4, ' '.join('%08x' % x for x in w[max(0, i - 3):i]), ' '.join('%08x' % x for x in w[i:i + 6]), ' '.join('%08x' % x for x in w[i + 6:i + 9])))
