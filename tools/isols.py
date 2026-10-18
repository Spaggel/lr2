import sys, struct
f = open(sys.argv[1], 'rb')
def rd(lba, n): f.seek(lba*2048); return f.read(n)
pvd = rd(16, 2048)
root = pvd[156:190]
def walk(lba, size, path, depth=0):
    data = rd(lba, size); i = 0
    while i < len(data):
        l = data[i]
        if l == 0: i = (i//2048+1)*2048; continue
        rec = data[i:i+l]; elba = struct.unpack('<I', rec[2:6])[0]; esz = struct.unpack('<I', rec[10:14])[0]
        flags = rec[25]; nl = rec[32]; name = rec[33:33+nl]
        i += l
        if name in (b'\x00', b'\x01'): continue
        n = name.decode('latin1').split(';')[0]
        p = path + '/' + n
        if flags & 2:
            print(f'{"DIR":>12} {p}')
            if depth < 4: walk(elba, esz, p, depth+1)
        else: print(f'{esz:>12} {p}')
walk(struct.unpack('<I', root[2:6])[0], struct.unpack('<I', root[10:14])[0], '')
