#!/usr/bin/env python3
"""ADT for the s8-qemu machine: tools/mkadt_dreamlte.py with N cpus (prefix of the dreamlte list).

  mkadt_qemu.py OUT.adt [--cpus 4] [--dram-size 0x20000000] [--canvas WxH]

Everything else (GICv2 0x10201000/0x10202000, 26 MHz timebase, dram-base 0x880000000, fbcon at 0xcc000000, UFS node) is the phone's.
Boot CPU = logical 0 = MPIDR 0 (platform.md); A53-only QEMU CPUs map to the first N Mongoose-numbered ADT nodes.
"""
import argparse, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))	# repo tools/
import adt  # noqa: E402
import mkadt_dreamlte as d  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument('output', type=Path)
ap.add_argument('--dram-base', type=lambda s: int(s, 0), default=0x880000000)

ap.add_argument('--cpus', type=int, default=4)
ap.add_argument('--dram-size', type=lambda s: int(s, 0), default=0x20000000)
ap.add_argument('--d22ap', default='/root/work/d22ap.adt', help='donor iPhone X ADT for the /product node (not in git)')
ap.add_argument('--canvas', default='1440x2960')
a = ap.parse_args()

import os
if os.path.exists(a.d22ap):
    d.mkadt.d22ap_product.__defaults__ = (a.d22ap,)
d.DRAM_SIZE = a.dram_size
d.DRAM_BASE = a.dram_base
root = d.build(tuple(int(v) for v in a.canvas.split('x')))
cpus = d.child(root, 'cpus')
cpus.children = cpus.children[:a.cpus]
blob = adt.serialize(root)
a.output.write_bytes(blob)
print(f'{a.output}: {len(blob)} bytes, {a.cpus} cpus (dreamlte identities: logical 0-3 = MPIDR 0-3), dram {d.DRAM_BASE:#x}+{a.dram_size:#x}')
