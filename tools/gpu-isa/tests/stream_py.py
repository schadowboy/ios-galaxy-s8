#!/usr/bin/env python3
# stream_py.py <capdir> [session]: exploration decoder for the compositor command stream (see analysis/compositor-stream.md).
# Replays the per-session deltas up to <session> (default: final state), walks every pass of the kernel command list (shmem_100)
# and prints the draw list. tests/stream_decode.c + core/s8gfx_stream.h are the production version.
import sys, os, re, struct, json
cap = sys.argv[1] if len(sys.argv) > 1 else 'data/capture17'
sess = int(sys.argv[2]) if len(sys.argv) > 2 else 99
regs = {}   # name -> (va, len)
for l in open(os.path.join(cap, 'index.txt')):
    m = re.match(r'session\s+(\d+) (\S+)\s+type (\d+) va 0x([0-9a-f]+) len 0x([0-9a-f]+)', l)
    if m: regs[m.group(2)] = (int(m.group(4), 16), int(m.group(5), 16))
bufs = {n: bytearray(ln) for n, (va, ln) in regs.items()}
for s in range(1, sess + 1):
    p = os.path.join(cap, 'session_%d.json' % s)
    if not os.path.exists(p): continue
    j = json.load(open(p))
    for n, lines in j.items():
        for o, h in lines.items():
            bufs[n][int(o, 16):int(o, 16) + 64] = bytes.fromhex(h)
mem = [(va, ln, bufs[n], n) for n, (va, ln) in regs.items() if va != 0]
shm = bytes(bufs['shmem_100'])

def find(va):
    for (b, ln, d, n) in mem:
        if b <= va < b + ln: return (b, ln, d, n)
    return None
def rd(va):
    r = find(va)
    if r is None: return None
    return struct.unpack_from('<I', r[2], va - r[0])[0]
def name(va):
    r = find(va)
    return '%s+%x' % (r[3], va - r[0]) if r else '?%x' % va
def H(v): return 'None' if v is None else '%08x' % v
def words(va, n): return ' '.join(H(rd(va + 4 * i)) for i in range(n))

def passes():
    out = []
    for o in range(0, len(shm) - 0x40, 4):
        a, b, c, d = struct.unpack_from('<IIII', shm, o)
        if (a, b, c, d) == (0x818, 0x7e8, 0x30, 1):
            p = struct.unpack_from('<Q', shm, o + 16)[0]
            base = o - 0x28
            w, h = struct.unpack_from('<II', shm, base + 0x690) if base + 0x698 <= len(shm) else (0, 0)
            out.append((base, p, w, h))
    return out

if __name__ == '__main__':
    for base, p, w, h in passes():
        print('=== pass block %x stream %x %dx%d' % (base, p, w, h))
        q = p
        for n in range(600):
            w0 = rd(q)
            if w0 is None: print('  <unmapped %x>' % q); break
            if w0 == 0x80000001:
                t = rd(q + 4); print('  %04x LINK -> %x' % (q & 0xffff, t)); q = 0x100000000 | t; continue
            if w0 == 0xc0000000:
                print('  %04x END' % (q & 0xffff)); break
            if (w0 & 0xffff00ff) == 1:
                c = (w0 >> 8) & 0xff; t = rd(q + 4)
                print('  %04x LOAD%d %08x %s: %s' % (q & 0xffff, c, t, name(0x100000000 | t), words(0x100000000 | t, c))); q += 8; continue
            print('  %04x raw %08x' % (q & 0xffff, w0)); q += 4
