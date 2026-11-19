import sys, os, glob, struct
# scanva.py <capdir> <hexbase> [span]: find dwords that look like references to VA range [base, base+span) in every final_*.bin:
#   lo/hi pair (lo dword followed by 1), VA>>4 form, and the 0x40000000|lo form
cap = sys.argv[1]
b = int(sys.argv[2], 16)
span = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x8000
for f in sorted(glob.glob(os.path.join(cap, 'final_*.bin'))):
    d = open(f, 'rb').read()
    n = len(d) // 4
    w = struct.unpack('<%dI' % n, d[:n * 4])
    lo = b & 0xffffffff
    for i in range(n - 1):
        if lo <= w[i] < lo + span and w[i + 1] == 1:
            print(os.path.basename(f), '%05x' % (i * 4), 'VA lo %08x hi 1' % w[i])
        if (b >> 4) <= w[i] < ((b + span) >> 4):
            print(os.path.basename(f), '%05x' % (i * 4), 'VA>>4 %08x next %08x' % (w[i], w[i + 1]))
        if (0x40000000 | lo) <= w[i] < (0x40000000 | (lo + span)):
            print(os.path.basename(f), '%05x' % (i * 4), '0x4-flag form %08x next %08x' % (w[i], w[i + 1]))
