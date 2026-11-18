#!/usr/bin/env python3
"""sym.py KERNEL ADDR... : nearest preceding symbol for each address (uses llvm-nm)."""
import bisect, subprocess, sys
out = subprocess.check_output(['llvm-nm', '-n', '--defined-only', sys.argv[1]], text=True)
syms = []
for l in out.splitlines():
    f = l.split(None, 2)
    if len(f) == 3 and f[1] in 'tTdDsSbB':
        syms.append((int(f[0], 16), f[2]))
addrs = [s[0] for s in syms]
for a in sys.argv[2:]:
    v = int(a, 0)
    i = bisect.bisect_right(addrs, v) - 1
    print(f'{v:#x} = {syms[i][1]}+{v - syms[i][0]:#x}' if i >= 0 else f'{v:#x} ?')
