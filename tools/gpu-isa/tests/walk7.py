import sys, struct
S = sys.argv[1] if len(sys.argv) > 1 else '8'
sys.argv = ['x', 'data/capture17', S]
exec(open('tests/stream_py.py').read().split("if __name__")[0])
# for every fragment state with a c0400038 block: print block header words and the bytes between block+0x40 and entry+8
seen = set()
d = bufs['res_17']
for o in range(0, len(d) - 24, 4):
    w0, w1 = struct.unpack_from('<II', d, o)
    if (w0 & 0xf) == 0xf and (w0 >> 16) == 0x11:
        n = 2 + (1 if w0 & 0x400 else 0)
        t = struct.unpack_from('<I', d, o + 4 * n)[0]
        if t == 0x108: n += 1; t = struct.unpack_from('<I', d, o + 4 * n)[0]
        if t != 0xc0400038: continue
        blk = struct.unpack_from('<I', d, o + 4 * n + 4)[0]
        ent = w1 & ~3
        if (blk, ent) in seen: continue
        seen.add((blk, ent))
        va = 0x100000000 | blk
        r = find(va)
        if r is None: continue
        data = bytes(r[2][va - r[0]: va - r[0] + (ent - blk) + 16])
        print('block %x entry %x (+%x) hdr %08x' % (blk, ent, ent - blk, w0))
        print('   hdr: ' + ' '.join('%08x' % struct.unpack_from('<I', data, i)[0] for i in range(0, 0x40, 4)))
        print('  code: ' + data[0x40:].hex(' '))
