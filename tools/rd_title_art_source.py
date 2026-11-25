#!/usr/bin/env python3
"""Makes tools/title_art.bin: the letters of "ENHANCED EDITION" and of the build number that rd_hybrid.py puts under the RAW DANGER
logo of the Enhanced Edition's title screen (TITLE.DAT picture 8). Only the letter shapes are kept here (one bit a pixel); the look
(the logo's own yellow, its red rim, the dark edge inside) is taken from the logo itself when the disc is made, so the words match
it and the game's palette. Needs Pillow and the Inter Black font; run it only to change the letters:
    python tools/rd_title_art_source.py [out.bin]

File: b"RDTA", version 1, count; then per letter: the character (1 byte), the size (1 byte: 0 the words, 1 the build number),
width, height (1 byte each), and the rows (each ceil(width / 8) bytes, the high bit first)."""
import os
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


if __name__ == "__main__" and (len(sys.argv) < 2 or sys.argv[1] not in ("psd", "png")):
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
    num0 = None
    num = words("v" + version, 10 * S, 0.9)
    # the same widths as the build's letters (tools/title_art.bin), 8 times: the words sit exactly where the game shows them
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import rd_hybrid
        art = rd_hybrid.load_title_art(os.path.join(os.path.dirname(os.path.abspath(__file__)), "title_art.bin"))
        width1 = lambda text, size: sum(len(art[(c, size)][0]) for c in text) + len(text) - 1
        main = main.resize((width1("ENHANCED EDITION", 0) * S, main.height), Image.LANCZOS)
        num = num.resize((width1("v" + version, 1) * S, num.height), Image.LANCZOS)
    except Exception as e:
        print("widths left as drawn:", e)
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

    # the colours the build uses: the logo's own yellows (US), white and grey (Japanese)
    yellows = []
    for y in range(363, 421):
        for x in range(368, 622):
            r, g, b, a = cols[d[pix + y * w + x]]
            if a >= 0x70 and r > 200 and g > 190 and b < 90: yellows.append((r, g, b))

    def grain(size):
        # the build's fill: one of the logo's yellows per game pixel, scattered the same way, each game pixel 8 x 8 here
        small = Image.new("RGB", (size[0] // S + 1, size[1] // S + 1))
        sp = small.load()
        k = 0
        for yy in range(small.height):
            for xx in range(small.width):
                sp[xx, yy] = yellows[(k * 7919) % len(yellows)] if yellows else (255, 236, 24)
                k += 1
        return small.resize((small.width * S, small.height * S), Image.NEAREST).crop((0, 0) + size)

    def right_bottom_edge(m, width):
        # the build's inner edge: the letters' right and bottom sides only, a game pixel wide (a light from the top left)
        right = ImageChops.offset(m, -width, 0)
        down = ImageChops.offset(m, 0, -width)
        return ImageChops.subtract(m, ImageChops.multiply(right, down))

    layers = []
    for part, m in (("Build number (drawn by the build: placement only)", nmask), ("Words", mask)):
        rim_w = 2 * S if us else S          # the build's rim: 2 game pixels (US), 1 (Japanese)
        rim = m.filter(ImageFilter.GaussianBlur(rim_w * 0.55)).point(lambda v: 255 if v > 10 else v * 25)   # round, not square
        inner = right_bottom_edge(m, S)
        if us:
            fill = grain(m.size).convert("RGBA")
            fill.putalpha(m)
            group = [layer(part + ": red rim", solid(rim, (236, 0, 0))), layer(part + ": yellow fill", fill),
                     layer(part + ": dark edge (right and bottom)", solid(inner, (96, 82, 0)))]
        else:
            group = [layer(part + ": soft dark edge", solid(rim, (24, 24, 32), 100)), layer(part + ": white", solid(m, (255, 255, 255))),
                     layer(part + ": grey edge (right and bottom)", solid(inner, (200, 204, 214)))]
        layers.append(nested_layers.Group(name=part, visible=True, layers=list(reversed(group))))
    # what the build makes of it in the game (640 x 448 game pixels, so 8 x 8 blocks here): a reference
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import rd_hybrid
        stamped = rd_hybrid.title_words(d, version, os.path.join(os.path.dirname(os.path.abspath(__file__)), "title_art.bin"))
        game = Image.new("RGBA", (CW, CH))
        gpx = game.load()
        for y in range(CH):
            for x in range(CW):
                r, g, b, a = cols[stamped[pix + (Y0 + y) * w + X0 + x]]
                gpx[x, y] = (r, g, b, min(255, a * 2))
        layers.append(layer("In the game: the build's result at 640 x 448, enlarged (reference; hide before you export)",
                            game.resize((CW * S, CH * S), Image.NEAREST), visible=False))
    except Exception as e:
        print("no in-game reference layer:", e)
    layers.append(layer("Guide: the title picture (hide it before you export)", guide))
    from pytoshop import enums
    out = nested_layers.nested_layers_to_psd(layers, color_mode=enums.ColorMode.rgb, size=(CW * S, CH * S), compression=enums.Compression.zip)
    with open(out_path, "wb") as f:
        out.write(f)
    print("written", out_path, CW * S, "x", CH * S)


# python tools/rd_title_art_source.py png <edited.psd> <tools/title_art_us.png | title_art_jp.png>
# The words as Photoshop drew them (its merged picture, layer effects included: bevel, stroke, shadow), cut out around the
# "Words" group: the guide, the in-game reference and the build number are left out whether they were hidden or not.
def png(psd_path, out_path, margin=40):
    import numpy as np
    from psd_tools import PSDImage
    p = PSDImage.open(psd_path)
    size = p.size
    merged = p.topil()
    if merged is None:
        merged = p.composite(force=True)   # (no merged picture saved: layer effects are not drawn then)
        print("no merged picture in the PSD (save it with Maximize compatibility): layer effects are left out")
    merged = np.asarray(merged.convert("RGBA")).copy()

    def content(layer):
        a = np.asarray(layer.composite(viewport=(0, 0) + size, force=True).convert("RGBA"))[..., 3]
        ys, xs = np.nonzero(a > 8)
        return (xs.min(), ys.min(), xs.max(), ys.max()) if len(xs) else None

    groups = {l.name.rstrip("\0"): l for l in p}
    words = next((l for n, l in groups.items() if n == "Words"), None)
    if words is None:
        sys.exit("no 'Words' group in " + psd_path)
    box = content(words)
    keep = np.zeros(merged.shape[:2], bool)
    x0, y0, x1, y1 = box
    keep[max(0, y0 - margin):y1 + margin + 1, max(0, x0 - margin):x1 + margin + 1] = True
    for n, l in groups.items():   # the build number and the guide pictures stay out
        if n.startswith("Build number"):
            b = content(l)
            if b: keep[max(0, b[1] - margin // 2):b[3] + margin // 2 + 1, max(0, b[0] - margin // 2):b[2] + margin // 2 + 1] = False
        elif n.startswith("Guide") or n.startswith("In the game"):
            a = np.asarray(l.composite(viewport=(0, 0) + size, force=True).convert("RGBA"))[..., 3]
            keep &= a < 8
    merged[~keep] = 0
    Image.fromarray(merged, "RGBA").save(out_path)
    print("written", out_path, "words at", box)


if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "png":
    png(sys.argv[2], sys.argv[3])

if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "psd":
    psd(sys.argv[2], sys.argv[3], sys.argv[4], *(sys.argv[5:6]))
