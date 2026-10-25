#!/usr/bin/env python3
"""Draws the gamepad button glyphs for the menus' button prompts.

The game draws prompt icons ("~~X Confirm ~~T Back") from 64x64 .MIP
textures; runtime/src/glyphs.c shows these instead while a gamepad is in
use.  They are stored under names the archive lists but the PC game never
loads (the PlayStation 2 version's button icons), so the HD-texture
mechanism (hdtex.c) can serve them from the "PAD GLYPHS" folder.

Each icon slot has a size scale (the table at 0x53fcf0: 1.4 for Enter and
the arrows, 1.2 for Esc), so glyphs are drawn smaller in the larger slots
to come out the same size on screen.

Needs Pillow.  Usage: tools/make_pad_glyphs.py [OUT_DIR] [--preview FILE.png]
"""
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

S = 256                       # drawing height; the texture is 64x64
DARK = (32, 32, 32, 255)
WHITE = (255, 255, 255, 255)
GREEN = (96, 196, 56, 255)    # Xbox A
RED = (232, 72, 52, 255)      # Xbox B
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

REF = 1.2                     # glyphs come out the size of one in a 1.2 slot
# (archive name, slot size scale, drawing)
GLYPHS = [
    ("CROSS", 1.4, ("button", "A", GREEN)),          # ~~X: confirm (Enter)
    ("CIRCLE", 1.2, ("button", "B", RED)),           # ~~T, ~~FRENCHT: back (Esc)
    ("L1", 1.4, ("dpad", "ud")),                     # ~~UPDOWN
    ("L2", 1.4, ("dpad", "lr")),                     # ~~LEFTRIGHT
    ("R1", 1.4, ("dpad", "udlr")),                   # ~~UDLR
]


def button(d, k, letter, colour):
    r = S * 0.46 * k
    cx, cy = S / 2, S / 2
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=WHITE)
    r2 = r - S * 0.075 * k
    d.ellipse([cx - r2, cy - r2, cx + r2, cy + r2], fill=DARK)
    f = ImageFont.truetype(FONT, int(S * 0.56 * k))
    d.text((cx, cy + S * 0.02 * k), letter, font=f, fill=colour, anchor="mm")


def dpad(d, k, dirs):
    cx, cy = S / 2, S / 2
    arm, half = S * 0.34 * k, S * 0.47 * k
    o = S * 0.06 * k                                # white outline
    for (pad, fill) in ((o, WHITE), (0, DARK)):
        a, h = arm / 2 + pad, half + pad - o
        d.rectangle([cx - a, cy - h, cx + a, cy + h], fill=fill)
        d.rectangle([cx - h, cy - a, cx + h, cy + a], fill=fill)
    t = arm * 0.32                                  # arrow half-width
    tip, base = half - S * 0.07 * k, half - S * 0.07 * k - t * 1.5
    for k, (dx, dy) in zip("udlr", ((0, -1), (0, 1), (-1, 0), (1, 0))):
        if k not in dirs:
            continue
        px, py = -dy, dx
        d.polygon([(cx + dx * tip, cy + dy * tip),
                   (cx + dx * base + px * t, cy + dy * base + py * t),
                   (cx + dx * base - px * t, cy + dy * base - py * t)], fill=WHITE)


def render(scale, spec):
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    k = REF / scale
    if spec[0] == "button":
        button(d, k, spec[1], spec[2])
    else:
        dpad(d, k, spec[1])
    return im


def premul_resize(im, size):
    """Downscale with premultiplied alpha, so edges do not go dark."""
    r, g, b, a = im.split()
    pm = Image.merge("RGBA", [Image.composite(c, Image.new("L", im.size, 0), a) for c in (r, g, b)] + [a])
    pm = pm.resize(size, Image.LANCZOS)
    px = pm.load()
    res = Image.new("RGBA", size)
    rp = res.load()
    for y in range(size[1]):
        for x in range(size[0]):
            cr, cg, cb, ca = px[x, y]
            rp[x, y] = (min(255, cr * 255 // ca), min(255, cg * 255 // ca), min(255, cb * 255 // ca), ca) if ca else (0, 0, 0, 0)
    return res


def mip_bytes(im):
    """A 32-bit .MIP: 38-byte header, then 7 levels (64..1) of BGRA rows, bottom-up."""
    head = bytearray(38)
    head[0], head[2], head[7] = 0x14, 0x02, 0x20
    struct.pack_into("<HHH4sH", head, 0x0c, 64, 64, 32, b"AIF1", 7)
    struct.pack_into("<H", head, 0x1a, 1)
    struct.pack_into("<H", head, 0x1e, 1)
    out = bytearray(head)
    for lv in range(7):
        n = 64 >> lv
        level = premul_resize(im, (n, n)) if n != im.size[0] else im
        for y in range(n - 1, -1, -1):
            for x in range(n):
                r, g, b, a = level.getpixel((x, y))
                out += bytes((b, g, r, a))
    return bytes(out)


def main():
    args, preview = sys.argv[1:], None
    if "--preview" in args:
        i = args.index("--preview")
        preview = args[i + 1]
        del args[i:i + 2]
    out = args[0] if args else os.path.join(os.path.dirname(__file__), "..", "PAD GLYPHS")
    dest = os.path.join(out, "GAME DATA", "FRONTEND", "CH BUILDER BITS")
    os.makedirs(dest, exist_ok=True)
    shots = []
    for name, scale, spec in GLYPHS:
        big = render(scale, spec)
        tex = premul_resize(big, (64, 64))
        with open(os.path.join(dest, name + ".MIP"), "wb") as f:
            f.write(mip_bytes(tex))
        n = int(64 * scale / REF)
        shots.append(tex.resize((int(n * 1.2), n)))          # as drawn at 16:10
        print(f"{name}.MIP")
    if preview:
        sheet = Image.new("RGBA", (sum(s.size[0] + 16 for s in shots) + 16, 96), (59, 59, 140, 255))
        x = 16
        for s in shots:
            sheet.alpha_composite(s, (x, 16))
            x += s.size[0] + 16
        sheet.resize((sheet.size[0] * 2, sheet.size[1] * 2), Image.NEAREST).save(preview)


main()
