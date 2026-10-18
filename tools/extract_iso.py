#!/usr/bin/env python3
"""Extract an ISO9660 image using its Joliet (long filename) directory tree.

usage: extract_iso.py <image.iso> <outdir>
"""
import os
import struct
import sys


def main():
    iso, out = sys.argv[1], sys.argv[2]
    f = open(iso, 'rb')

    def rd(lba, n):
        f.seek(lba * 2048)
        return f.read(n)

    # Find the Joliet supplementary volume descriptor (type 2, UCS-2 escape).
    root, joliet = None, False
    for lba in range(16, 64):
        vd = rd(lba, 2048)
        if vd[0] == 255:
            break
        if vd[0] == 2 and vd[88:91] in (b'%/@', b'%/C', b'%/E'):
            root, joliet = vd[156:190], True
            break
        if vd[0] == 1 and root is None:
            root = vd[156:190]

    def name_of(raw):
        n = raw.decode('utf-16-be') if joliet else raw.decode('latin1')
        return n.split(';')[0]

    count = 0

    def walk(lba, size, path):
        nonlocal count
        data = rd(lba, size)
        i = 0
        while i < len(data):
            ln = data[i]
            if ln == 0:
                i = (i // 2048 + 1) * 2048
                continue
            rec = data[i:i + ln]
            i += ln
            elba, esz = struct.unpack('<I', rec[2:6])[0], struct.unpack('<I', rec[10:14])[0]
            raw = rec[33:33 + rec[32]]
            if raw in (b'\x00', b'\x01'):
                continue
            name = name_of(raw)
            dst = os.path.join(path, name)
            if rec[25] & 2:
                os.makedirs(dst, exist_ok=True)
                walk(elba, esz, dst)
            else:
                f.seek(elba * 2048)
                with open(dst, 'wb') as o:
                    left = esz
                    while left:
                        chunk = f.read(min(left, 1 << 20))
                        o.write(chunk)
                        left -= len(chunk)
                count += 1

    os.makedirs(out, exist_ok=True)
    walk(struct.unpack('<I', root[2:6])[0], struct.unpack('<I', root[10:14])[0], out)
    print(f'extracted {count} files to {out} ({"Joliet" if joliet else "ISO9660"} names)')


if __name__ == '__main__':
    main()
