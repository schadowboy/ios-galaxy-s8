import importlib.util, os, struct, sys

# export_scenes.py <capture dir> <out file>: the fragment program bytes of the scenes (res_3 + 0xc80 as of each session) as  [u32 scene][u32 len][bytes] records
here = r'C:\github\ios-s8\tools'
spec = importlib.util.spec_from_file_location('agxd', os.path.join(here, 'agx-disasm.py'))
agxd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(agxd)
d, out = sys.argv[1], sys.argv[2]
f = open(out, 'wb')
for s in range(1, 36):
    mem = agxd.load_region(d, 'res_3', s)
    a = 0xc80
    raw = bytearray()
    for _ in range(80):
        b = agxd.byte_at(mem, a, 16)
        name, ln = agxd.decode(b, 0)
        raw += b[:ln]
        a += ln
        if name in ('end_g10', 'stop') or name is None:
            break
    f.write(struct.pack('<II', s, len(raw)) + bytes(raw))
f.close()
print('exported', out)
