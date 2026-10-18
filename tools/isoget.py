import sys, struct
f = open(sys.argv[1], 'rb'); want = sys.argv[2].upper().strip('/').split('/'); out = sys.argv[3]
def rd(lba, n): f.seek(lba*2048); return f.read(n)
pvd = rd(16, 2048); r = pvd[156:190]
lba, size = struct.unpack('<I', r[2:6])[0], struct.unpack('<I', r[10:14])[0]
for part in want:
    data = rd(lba, size); i = 0; found = None
    while i < len(data):
        l = data[i]
        if l == 0: i = (i//2048+1)*2048; continue
        rec = data[i:i+l]; i += l
        n = rec[33:33+rec[32]].decode('latin1').split(';')[0].upper()
        if n == part: found = (struct.unpack('<I', rec[2:6])[0], struct.unpack('<I', rec[10:14])[0]); break
    lba, size = found
open(out, 'wb').write(rd(lba, size))
