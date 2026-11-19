import sys,struct
f=sys.argv[1]; a=int(sys.argv[2],0) if len(sys.argv)>2 else 0; n=int(sys.argv[3],0) if len(sys.argv)>3 else 0x400
d=open(f,'rb').read()
for o in range(a,min(len(d),a+n),32):
    w=struct.unpack_from('<8I',d,o) if o+32<=len(d) else ()
    if any(w): print('%05x: '%o+' '.join('%08x'%x for x in w))
