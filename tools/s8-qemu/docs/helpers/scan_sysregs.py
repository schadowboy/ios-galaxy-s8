#!/usr/bin/env python3
"""Disassemble the executable segments of the A5XELTE kernel Mach-O and list every MRS/MSR of an
implementation-defined (s3_*) or otherwise notable system register, with its address (for platform.md).
usage: scan_sysregs.py KERNEL OUTDIR   (needs aarch64-linux-gnu-objdump)"""
import os, re, struct, subprocess, sys, collections

kern, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
buf = open(kern, 'rb').read()
magic, cputype, cpusub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<8I', buf, 0)
off = 32
segs = []
for _ in range(ncmds):
    cmd, size = struct.unpack_from('<II', buf, off)
    if cmd == 0x19:
        name = buf[off + 8:off + 24].split(b'\0')[0].decode()
        vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4Q', buf, off + 24)
        nsects = struct.unpack_from('<I', buf, off + 64)[0]
        so = off + 72
        for i in range(nsects):
            sect = buf[so:so + 16].split(b'\0')[0].decode()
            addr, sz, foff = struct.unpack_from('<QQI', buf, so + 32)
            flg = struct.unpack_from('<I', buf, so + 64)[0]
            segs.append((name, sect, addr, sz, foff, flg))
            so += 80
    off += size
print('sections:')
for s in segs:
    print('  %-18s %-16s va=%#x size=%#x' % (s[0], s[1], s[2], s[3]))
hits = collections.defaultdict(list)
for name, sect, addr, sz, foff, flg in segs:
    if not (flg & 0x80000000 or sect in ('__text', '__TEXT_EXEC')) or sz == 0 or foff == 0:
        continue
    if name not in ('__TEXT_EXEC', '__TEXT', '__KLD', '__BOOTDATA'):
        continue
    raw = os.path.join(out, 'sect_%s_%s.bin' % (name, sect))
    open(raw, 'wb').write(buf[foff:foff + sz])
    p = subprocess.run(['aarch64-linux-gnu-objdump', '-D', '-b', 'binary', '-m', 'aarch64', '--adjust-vma=%#x' % addr, raw],
                       capture_output=True, text=True)
    for line in p.stdout.splitlines():
        m = re.search(r'^\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(mrs|msr)\s+(.*)$', line)
        if m:
            ops = m.group(3)
            sysreg = ops.split(',')[0] if m.group(2) == 'msr' else ops.split(',')[-1]
            hits[sysreg.strip()].append(int(m.group(1), 16))
    os.remove(raw)
with open(os.path.join(out, 'sysregs.txt'), 'w') as f:
    for k in sorted(hits, key=lambda k: -len(hits[k])):
        f.write('%-28s n=%-4d first=%s\n' % (k, len(hits[k]), ' '.join('%#x' % a for a in hits[k][:3])))
print(open(os.path.join(out, 'sysregs.txt')).read())
