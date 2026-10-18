#!/usr/bin/env python3
"""Write the original game's icon as an .ico file (for the Windows build).

usage: extract_icon.py orig/LEGORacers2.exe runtime/res/lr2.ico
Needs pefile (pip install pefile).
"""
import struct
import sys

import pefile

RT_ICON, RT_GROUP_ICON = 3, 14


def main(exe, out):
    pe = pefile.PE(exe)
    res = {t.id: t for t in pe.DIRECTORY_ENTRY_RESOURCE.entries}

    def data(entry):
        d = entry.directory.entries[0].data.struct
        return pe.get_data(d.OffsetToData, d.Size)

    icons = {e.id: data(e) for e in res[RT_ICON].directory.entries}
    group = data(res[RT_GROUP_ICON].directory.entries[0])
    _, kind, count = struct.unpack_from("<HHH", group)
    header = struct.pack("<HHH", 0, kind, count)
    entries, images = b"", b""
    offset = 6 + 16 * count
    for i in range(count):
        w, h, colors, _, planes, bits, size, icon_id = struct.unpack_from("<BBBBHHIH", group, 6 + 14 * i)
        img = icons[icon_id]
        entries += struct.pack("<BBBBHHII", w, h, colors, 0, planes, bits, len(img), offset)
        images += img
        offset += len(img)
    with open(out, "wb") as f:
        f.write(header + entries + images)


if __name__ == "__main__":
    main(*sys.argv[1:3])
