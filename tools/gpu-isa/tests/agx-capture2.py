import re, sys, os, json

# agx-capture2.py <decoded kernel log> <out dir>: rebuild delta dumps. Header "S8AGX: D <session> <what> <id> type <t> va <va> len <len>", lines
# "S8AGX: L <off> <64 bytes hex>". State per (what, id) persists across sessions; per session the changed lines are saved to
# <out>/session_<n>.json ({ "<what>_<id>": { "<off>": "<hex>" } }) and the final state of every region to <out>/final_<what>_<id>.bin.
log, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
hdr = re.compile(r'S8AGX: D (\d+) (\w+) (\d+) type (\d+) va 0x([0-9a-f]+) len 0x([0-9a-f]+)')
line = re.compile(r'S8AGX: L ([0-9a-f]+) ([0-9a-f]{128})')
state = {}
meta = {}
sessions = {}
cur = None
for l in open(log, errors='replace'):
    m = hdr.search(l)
    if m:
        s, what, rid, typ, va, ln = int(m.group(1)), m.group(2), int(m.group(3)), int(m.group(4)), int(m.group(5), 16), int(m.group(6), 16)
        key = '%s_%d' % (what, rid)
        state.setdefault(key, bytearray(ln))
        meta[key] = (typ, va, ln)
        sessions.setdefault(s, {}).setdefault(key, {})
        cur = (s, key)
        continue
    m = line.search(l)
    if m and cur is not None:
        off = int(m.group(1), 16)
        b = bytes.fromhex(m.group(2))
        state[cur[1]][off:off + 64] = b
        sessions[cur[0]][cur[1]][off] = m.group(2)
for s, regs in sorted(sessions.items()):
    json.dump({k: {'%x' % o: h for o, h in v.items()} for k, v in regs.items()}, open(os.path.join(out, 'session_%d.json' % s), 'w'))
for key, data in state.items():
    open(os.path.join(out, 'final_%s.bin' % key), 'wb').write(bytes(data))
with open(os.path.join(out, 'index.txt'), 'w') as f:
    for s, regs in sorted(sessions.items()):
        for key, v in regs.items():
            typ, va, ln = meta[key]
            f.write('session %2d %-10s type %d va 0x%x len 0x%x: %d changed line(s)\n' % (s, key, typ, va, ln, len(v)))
print(len(sessions), 'sessions,', len(state), 'regions ->', out)
