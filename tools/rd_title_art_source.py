#!/usr/bin/env python3
"""Makes tools/title_art.bin: the letters of "ENHANCED EDITION" and of the build number that rd_hybrid.py puts under the RAW DANGER
logo of the Enhanced Edition's title screen (TITLE.DAT picture 8). Only the letter shapes are kept here (one bit a pixel); the look
(the logo's own yellow, its red rim, the dark edge inside) is taken from the logo itself when the disc is made, so the words match
it and the game's palette. Needs Pillow and the Inter Black font; run it only to change the letters:
    python tools/rd_title_art_source.py [out.bin]

File: b"RDTA", version 1, count; then per letter: the character (1 byte), the size (1 byte: 0 the words, 1 the build number),
width, height (1 byte each), and the rows (each ceil(width / 8) bytes, the high bit first)."""
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

FONT = "/usr/share/fonts/opentype/inter/Inter-Black.otf"
SIZES = {0: 14, 1: 10}           # cap height in pixels: the words, the build number
SQUEEZE = {0: 0.68, 1: 0.9}      # narrower, like the logo's letters (the digits less, to stay readable)
SETS = {0: "ENHACDITO ", 1: "v0123456789. "}


def glyph(ch, cap, squeeze):
    big = 8                      # drawn 8 times larger, then made small: clean edges at a few pixels
    font = ImageFont.truetype(FONT, cap * big * 100 // 72)
    box = font.getbbox("H")
    capb = box[3] - box[1]
    font = ImageFont.truetype(FONT, int(font.size * cap * big / capb))
    top = font.getbbox("H")[1]
    w = max(1, int(font.getlength(ch) + 2))
    img = Image.new("L", (w, cap * big + 4 * big), 0)
    ImageDraw.Draw(img).text((0, -top), ch, font=font, fill=255)
    img = img.crop((0, 0, w, cap * big))
    nw = max(1, round(w * squeeze / big)) if ch != " " else max(2, round(cap * 0.35))
    small = img.resize((nw, cap), Image.LANCZOS)
    bits = [[1 if small.getpixel((x, y)) >= 110 else 0 for x in range(nw)] for y in range(cap)]
    if ch == ".":                # a dot of at least 2 x 2 at the baseline
        bits = [[0, 0] for _ in range(cap - 2)] + [[1, 1], [1, 1]]
    if ch not in " .":           # trim empty columns at the sides
        cols = [x for x in range(nw) if any(bits[y][x] for y in range(cap))]
        bits = [row[cols[0]:cols[-1] + 1] for row in bits]
    return bits


def main():
    out = bytearray(b"RDTA") + struct.pack("<HH", 1, 0)
    n = 0
    for size, chars in SETS.items():
        for ch in chars:
            bits = glyph(ch, SIZES[size], SQUEEZE[size])
            w, h = len(bits[0]), len(bits)
            out += struct.pack("<BBBB", ord(ch), size, w, h)
            for row in bits:
                for b in range(0, w, 8):
                    v = 0
                    for i, bit in enumerate(row[b:b + 8]):
                        v |= bit << (7 - i)
                    out.append(v)
            n += 1
    struct.pack_into("<H", out, 6, n)
    open(sys.argv[1] if len(sys.argv) > 1 else "title_art.bin", "wb").write(out)
    print(f"{n} letters, {len(out)} bytes")


if __name__ == "__main__" and (len(sys.argv) < 2 or sys.argv[1] != "psd"):
    main()


# ---- the layered PSD for refining the words by hand (v0.8.7) ----
# python tools/rd_title_art_source.py psd <TITLE.DAT> <us|jp> <out.psd> [version]
# The canvas is the title picture's corner from x 300, y 330 (340 x 118) at 8 times the size (2720 x 944), as rd_hybrid.py reads
# tools/title_art_us.png / title_art_jp.png: hide the guide and the build number, export the rest as a PNG of that size.
def psd(title_dat, style, out_path, version="0.8.7"):
    import numpy as np
    from PIL import ImageChops, ImageFilter
    from pytoshop.user import nested_layers
    S, X0, Y0, CW, CH = 8, 300, 330, 340, 118
    d = open(title_dat, "rb").read()
    tims, p = [], 0
    while True:
        p = d.find(b"TIM2", p)
        if p < 0: break
        tims.append(p); p += 4
    t = tims[8]
    q = t + (16 if d[t + 5] == 0 else 128)
    total, clut, img, hsz, ncol, fmt, mip, ctype, itype, w, h = struct.unpack_from("<IIIHHBBBBHH", d, q)
    pix, pal = q + hsz, q + hsz + img
    raw = [tuple(d[pal + 4 * i:pal + 4 * i + 4]) for i in range(256)]
    cols = [raw[(i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)] for i in range(256)]
    guide = Image.new("RGBA", (w, h))
    gp = guide.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = cols[d[pix + y * w + x]]
            gp[x, y] = (r, g, b, min(255, a * 2))
    guide = guide.crop((X0, Y0, X0 + CW, Y0 + CH)).resize((CW * S, CH * S), Image.LANCZOS)
    us = style == "us"

    def words(text, cap, squeeze, tracking=0.12):
        font = ImageFont.truetype(FONT, 200)
        box = font.getbbox("H")
        font = ImageFont.truetype(FONT, int(200 * cap / (box[3] - box[1])))
        top = font.getbbox("H")[1]
        glyphs = []
        for ch in text:
            if ch == " ":
                glyphs.append(Image.new("L", (int(cap * 0.35), cap), 0))
                continue
            g = Image.new("L", (int(font.getlength(ch)) + 20, cap + 60), 0)
            ImageDraw.Draw(g).text((10, 30 - top), ch, font=font, fill=255)
            g = g.crop((g.getbbox()[0], 30, g.getbbox()[2], 30 + cap))
            glyphs.append(g.resize((max(1, int(g.width * squeeze)), cap), Image.LANCZOS))
        gap = int(cap * tracking)
        im = Image.new("L", (sum(g.width for g in glyphs) + gap * (len(glyphs) - 1), cap), 0)
        x = 0
        for g in glyphs:
            im.paste(g, (x, 0)); x += g.width + gap
        return im

    main = words("ENHANCED EDITION", 14 * S, 0.68)
    num = words("v" + version, 10 * S, 0.9)
    gap = 6 * S
    right = (618 - X0) * S
    base = ((441 if us else 360) - Y0) * S
    mask = Image.new("L", (CW * S, CH * S), 0)
    x = right - main.width - gap - num.width
    mask.paste(main, (x, base - main.height))
    nmask = Image.new("L", mask.size, 0)
    nmask.paste(num, (x + main.width + gap, base - num.height))
    if us:   # leaning like the logo: one pixel up every 40 to the right
        lean = lambda m: m.transform(m.size, Image.AFFINE, (1, 0, 0, 1.0 / 40, 1, -(x / 40.0)), Image.BICUBIC)
        mask, nmask = lean(mask), lean(nmask)

    def layer(name, rgba, visible=True):
        a = np.asarray(rgba)
        return nested_layers.Image(name=name, visible=visible, opacity=255, top=0, left=0, bottom=a.shape[0], right=a.shape[1],
                                   channels={0: a[:, :, 0].copy(), 1: a[:, :, 1].copy(), 2: a[:, :, 2].copy(), -1: a[:, :, 3].copy()})

    def solid(m, colour, alpha=255):
        im = Image.new("RGBA", m.size, colour + (0,))
        im.putalpha(m.point(lambda v: v * alpha // 255))
        return im

    layers = []
    for part, m in (("Build number (drawn by the build: placement only)", nmask), ("Words", mask)):
        rim_w, edge_w = (16, 6) if us else (8, 4)
        rim = m.filter(ImageFilter.GaussianBlur(rim_w * 0.6)).point(lambda v: 255 if v > 12 else v * 20)   # round, not square
        inner = ImageChops.subtract(m, m.filter(ImageFilter.MinFilter(2 * edge_w + 1)))
        if us:
            # the logo's grain: its yellow letters, made big and laid over the words
            grain = guide.crop((70 * S, 35 * S, 320 * S, 88 * S)).resize(m.size, Image.LANCZOS).convert("RGB")
            fill = Image.new("RGBA", m.size, (255, 230, 0, 0))
            fill.paste(Image.blend(Image.new("RGB", m.size, (255, 236, 24)), grain, 0.3), (0, 0))
            fill.putalpha(m)
            group = [layer(part + ": red rim", solid(rim, (236, 0, 0))), layer(part + ": yellow fill", fill),
                     layer(part + ": dark edge inside", solid(inner, (96, 82, 0)))]
        else:
            group = [layer(part + ": soft dark edge", solid(rim, (24, 24, 32), 100)), layer(part + ": white", solid(m, (255, 255, 255))),
                     layer(part + ": grey edge inside", solid(inner, (200, 204, 214)))]
        layers.append(nested_layers.Group(name=part, visible=part == "Words", layers=list(reversed(group))))
    layers.append(layer("Guide: the title picture (hide it before you export)", guide))
    from pytoshop import enums
    out = nested_layers.nested_layers_to_psd(layers, color_mode=enums.ColorMode.rgb, size=(CW * S, CH * S), compression=enums.Compression.zip)
    with open(out_path, "wb") as f:
        out.write(f)
    print("written", out_path, CW * S, "x", CH * S)


if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "psd":
    psd(sys.argv[2], sys.argv[3], sys.argv[4], *(sys.argv[5:6]))
