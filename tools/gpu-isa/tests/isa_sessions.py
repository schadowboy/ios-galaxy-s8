import json, os, sys, re
# isa_sessions.py <capture dir> [first] [last]: print the changed res_3 64-byte lines of each session (program heap diffs)
d = sys.argv[1]
a = int(sys.argv[2]) if len(sys.argv) > 2 else 1
b = int(sys.argv[3]) if len(sys.argv) > 3 else 999
for n in range(a, b + 1):
    p = os.path.join(d, 'session_%d.json' % n)
    if not os.path.exists(p):
        continue
    s = json.load(open(p))
    r = s.get('res_3', {})
    print('== session', n, 'res_3 lines', len(r))
    for o in sorted(r, key=lambda x: int(x, 16)):
        h = r[o]
        print(' %5s' % o, ' '.join(h[i:i + 2] for i in range(0, 128, 2)))
