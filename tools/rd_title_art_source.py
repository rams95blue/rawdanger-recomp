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


if __name__ == "__main__":
    main()
