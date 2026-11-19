import json, sys
for s in range(2, 11):
    try: j = json.load(open('data/capture17/session_%d.json' % s))
    except Exception: continue
    for k in sys.argv[1:]:
        if k in j: print('session', s, k, sorted(int(o, 16) for o in j[k]))
