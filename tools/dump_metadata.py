# Lists every class with its fields and methods from a Unity il2cpp global-metadata.dat (version 29-31).
# usage: python3 tools/dump_metadata.py global-metadata.dat dump.txt
# The mod itself does not need this: it finds everything by name at run time. This is only to check names after a game update.
import struct, sys
d=open((sys.argv[1] if len(sys.argv) > 1 else 'global-metadata.dat'),'rb').read()
H=struct.unpack('<'+'i'*60,d[:240])
def sec(i): return H[2+2*i],H[3+2*i]
strOff,strSize=sec(2)
methOff,methSz=sec(5)
fieldOff,_=sec(11)
tdOff,tdSz=sec(19)
paramOff,_=sec(10)
def S(i):
    a=strOff+i; b=d.index(b'\0',a); return d[a:b].decode('utf8','replace')
n=tdSz//88
types=[]
for i in range(n):
    t=struct.unpack_from('<7iI8iHHHHHHHHII',d,tdOff+i*88)
    types.append(t)
# t: 0 name,1 ns,2 byval,3 declaring,4 parent,5 elem,6 gc,7 flags,8 fieldStart,9 methodStart,10 eventStart,11 propStart,12 nestedStart,13 ifStart,14 vtStart,15 ifoStart,16 mcount,17 pcount,18 fcount ...
out=open(sys.argv[2] if len(sys.argv) > 2 else 'dump.txt','w')
for i,t in enumerate(types):
    name=S(t[0]); ns=S(t[1])
    full=(ns+'.' if ns else '')+name
    par=types[t[4]] if t[4]>=0 and t[4]<len(types) else None
    out.write(f"\nclass {full}  [#{i}] fields={t[18]} methods={t[16]}\n")
    for f in range(t[18]):
        nm,ty,tok=struct.unpack_from('<iiI',d,fieldOff+(t[8]+f)*12)
        out.write(f"  F {S(nm)}\n")
    for m in range(t[16]):
        mm=struct.unpack_from('<7iHHHH',d,methOff+(t[9]+m)*36)
        out.write(f"  M {S(mm[0])}({mm[10]}) flags={mm[7]:#x}\n")
out.close()
print(n,'types')
