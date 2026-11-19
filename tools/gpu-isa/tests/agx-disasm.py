import json, os, re, struct, sys, runpy

# agx-disasm.py <capture dir> <region> <hex byte offset> [max instructions] : disassemble AGX (A11 / G10) shader code captured by the AGX stand-in.
# Encodings come from Mesa's Asahi compiler tables (agx_opcodes.py, the M1 / G13 generation); G10_OVERRIDES holds what is confirmed to differ on the A11.
# The captured regions are merged over all sessions (latest line wins), like tools/agx-heap.py.
MESA = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'agx_opcodes_mesa.py')   # copy of Mesa src/asahi/compiler/agx_opcodes.py (MIT, Alyssa Rosenzweig)
ns = runpy.run_path(MESA)
opcodes = ns['opcodes']

L = 1 << 15
# name -> (exact, mask, length_short, length_long): confirmed A11 differences (decoded from captured programs and the known test kernels)
G10_OVERRIDES = {
    'mov_imm': (0x52, 0xFF, 6, 6),         # 6 bytes: 52 <reg*2> <imm32> (green test: r0=0 r2=0 r1=1.0 r3=1.0)
    'ld_tile': (0x79, 0x7F, 8, 8),         # 8 bytes, same shape as st_tile (09 ..); reads the tile (the blend's destination colour)
    # calibrated on the single-operation render scenes (capture8, tools/agx-scenes.py); the float ops are 0x0A | (n << 6) on the A11
    'iter': (0x51, 0xFF, 8, 8),            # varying interpolation: 51 <dst> <coeff regs> ... (8 B)
    'fmul': (0x4A, 0xFF, 6, 6),            # 4a <dst> <src0> <src1 / imm form> (in.col * 0.5, in.col * in.col)
    'fmul_l': (0x1A, 0xFF, 8, 8),          # long form of fmul seen with high source registers
    'fadd': (0x8A, 0xFF, 6, 6),
    'ffma': (0xCA, 0xFF, 8, 8),            # fma(a, b, c), also saturate-scale forms
    'funop': (0x0A, 0xFF, 6, 6),           # unary function family: rcp, rsqrt, exp2, log2, sin ... (function code in the later bytes)
    'mov_imm_x': (0xD2, 0xFF, 6, 6),       # immediate load with an extra flag (d2 04 .. 80 3e = 0.25, d2 0a .. 80 7f = +inf)
    'signal_pix': (0x58, 0xFF, 4, 4),      # seen after discard
    'get_sr_g10': (0xE2, 0xFF, 4, 4),      # e2 06 10 04: thread id / position fetch (M1 get_sr is 0x72)
    'fcmpsel_g10': (0xFA, 0xFF, 8, 8),     # fa 09 ..: compare-and-select (min/max scene)
    'texture_sample': (0x21, 0xFF, 10, 10),  # 21 01 00 41 0f c8 00 80 00 20: mask nibble in byte 4 (0f = RGBA, 07 = RGB), coordinate register c8; nearest/linear
                                             # produce identical code (the filter is sampler STATE). Followed by 38 00 (wait for the result). M1: 0x31.
}
# the A11's end-of-program: "08 01 00 00" after the final st_tile (G13: stop = 0x88)
EXTRA = [('end_g10', 0x0108, 0xFFFF, 4, 4)]

table = []
for name, o in opcodes.items():
    e = o.encoding
    if e is None:
        continue
    exact, mask, ls, ll = e.exact, e.mask, e.length_short, e.length_short + (4 if e.length_short > 8 else 2) if e.extensible else e.length_short
    if name in G10_OVERRIDES:
        exact, mask, ls, ll = G10_OVERRIDES[name]
    table.append((name, exact, mask, ls, ll))
for name, exact, mask, ls, ll in EXTRA:
    table.append((name, exact, mask, ls, ll))
# confirmed A11 encodings first (they win over colliding M1 opcodes), then the rest, most specific mask first
table.sort(key=lambda t: (0 if t[0] in G10_OVERRIDES else 1, -bin(t[2]).count('1')))


def decode(buf, off):
    w = int.from_bytes(buf[off:off + 8].ljust(8, b'\0'), 'little')
    # the A11 iter comes as 8 bytes (51 <dst> <ch> 80 ..) or as a 4-byte form (51 03 81 00) in front of a texture sample (21 ..); heuristic: cache bit of byte 1
    if buf[off] == 0x51 and (buf[off + 1] & 1):
        return 'iter_short', 4
    for name, exact, mask, ls, ll in table:
        if (w & mask) == exact:
            n = ll if (ll != ls and (w & L)) else ls
            return name, n
    return None, 2


def load_region(d, reg, upto=10 ** 9):
    mem = {}
    nums = sorted(int(re.match(r'session_(\d+)\.json', f).group(1)) for f in os.listdir(d) if re.match(r'session_\d+\.json', f))
    for n in nums:
        if n > upto:
            break
        s = json.load(open(os.path.join(d, 'session_%d.json' % n)))
        for o, hx in s.get(reg, {}).items():
            mem[int(o, 16)] = bytes.fromhex(hx)
    return mem


def byte_at(mem, a, n):
    out = bytearray()
    for i in range(n):
        line = mem.get((a + i) & ~63)
        out.append(line[(a + i) & 63] if line else 0)
    return bytes(out)


if __name__ == '__main__':
    d, reg, off = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
    maxn = int(sys.argv[4]) if len(sys.argv) > 4 else 64
    upto = int(sys.argv[5]) if len(sys.argv) > 5 else 10 ** 9     # merge only sessions up to this one (shows the program as of that submit)
    mem = load_region(d, reg, upto)
    a = off
    for _ in range(maxn):
        b = byte_at(mem, a, 16)
        name, n = decode(b, 0)
        print('%06x: %-34s %s' % (a, b[:n].hex(' '), name or '???'))
        if name in ('end_g10', 'stop'):
            break
        a += n
