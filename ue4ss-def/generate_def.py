import struct, sys, os

path = sys.argv[1]
data = open(path,'rb').read()

e_lfanew = struct.unpack_from('<I', data, 0x3C)[0]
assert data[e_lfanew:e_lfanew+4] == b'PE\0\0', "not PE"
coff = e_lfanew + 4
machine, nsec, _, _, _, optsize, _ = struct.unpack_from('<HHIIIHH', data, coff)
opt = coff + 20
magic = struct.unpack_from('<H', data, opt)[0]
assert magic == 0x20B, f"expected PE32+, got {hex(magic)}"
# PE32+ data directory starts at offset 112
ddoff = opt + 112
exp_rva, exp_size = struct.unpack_from('<II', data, ddoff)

# section table
sect = opt + optsize
sections = []
for i in range(nsec):
    o = sect + i*40
    name = data[o:o+8].rstrip(b'\0').decode('latin1')
    vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', data, o+8)
    sections.append((name, vaddr, vsize, rawptr, rawsize))

def rva2off(rva):
    for name, vaddr, vsize, rawptr, rawsize in sections:
        size = max(vsize, rawsize)
        if vaddr <= rva < vaddr + size:
            return rawptr + (rva - vaddr)
    return None

eo = rva2off(exp_rva)
chars, tds, maj, minr, name_rva, base = struct.unpack_from('<IIHHII', data, eo)
nfunc, nnames, addr_funcs, addr_names, addr_ords = struct.unpack_from('<IIIII', data, eo+20)

no = rva2off(addr_names)
oo = rva2off(addr_ords)
fo = rva2off(addr_funcs)

funcs = struct.unpack_from(f'<{nfunc}I', data, fo)
names = []
for i in range(nnames):
    nr = struct.unpack_from('<I', data, no + i*4)[0]
    ordv = struct.unpack_from('<H', data, oo + i*2)[0]
    p = rva2off(nr)
    end = data.index(b'\0', p)
    nm = data[p:end].decode('latin1')
    fwd = funcs[ordv]
    is_fwd = exp_rva <= fwd < exp_rva + exp_size
    names.append((nm, ordv, is_fwd))

print(f"# {os.path.basename(path)}")
print(f"# machine={hex(machine)} sections={nsec} exports_total={nfunc} exported_names={nnames}")
print(f"# exported-func entries: {sum(1 for _,_,f in names if not f)}")
print(f"# forwarders: {sum(1 for _,_,f in names if f)}")
sys.stdout.flush()

with open(sys.argv[2],'w',encoding='latin1') as f:
    f.write(f"LIBRARY UE4SS\n")
    f.write("EXPORTS\n")
    for nm, _, _ in sorted(names):
        f.write(f"    {nm}\n")

print("wrote", sys.argv[2], os.path.getsize(sys.argv[2]), "bytes")
# show the ones our mod needs
need = ['IsEngineTickAvailable','RegisterEngineTickPreCallback','UnregisterCallback']
allnames = [n for n,_,_ in names]
for k in need:
    hits = [n for n in allnames if k in n]
    print(f"  {k}: {len(hits)}")
    for h in hits[:4]: print("     ", h)
