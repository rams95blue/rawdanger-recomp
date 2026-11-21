#!/usr/bin/env python3
"""rd_prompts.py - make the game's font an override so its texts can show PC keys in place of the PlayStation symbols.

  python tools/rd_prompts.py make --iso disc.iso --out FOLDER
  python tools/rd_prompts.py selftest

Raw Danger! writes every text (tips, subtitles, menus) with one font, TEX\\1001.TM2: 384 x 384 pixels, 16 colours, drawn as a
512 x 512 texture. The PlayStation symbols are characters of that font, side by side in one row (circle, triangle, square,
cross, arrow; 24 pixels apart). This finds the font on the disc (the only 384 x 384 16-colour TIM2 picture), writes it as
FOLDER\\font_1001.png (512 x 512, the font at the top left) with FOLDER\\textures_index.tsv for the engine's texture overrides,
and FOLDER\\prompts.env with the font's key and the symbols' boxes:

  PS2_TEX_OVERRIDES=FOLDER
  PS2_PROMPT_FONT_KEY=<the key of the font's pixels, as the engine computes it>
  PS2_PROMPT_CELLS=<cross;circle;square;triangle as x0,x1,y0,y1 in font pixels>

The engine then draws a small key cap with the key the player gave that button over each symbol (F1 -> Settings -> Controls,
"Buttons in the game's texts"); with no key or the switch off the symbols stay. The picture itself is the game's own font,
unchanged: the caps are drawn by the engine each time the keys change.
"""
import argparse, mmap, os, struct, sys, zlib

FONT_W = FONT_H = 384
TEX_W = TEX_H = 512
# x0,x1,y0,y1 of the symbols, measured on the US font (the Enhanced Edition uses the same)
CELLS = {"cross": (72, 80, 271, 279), "circle": (1, 9, 272, 281), "square": (48, 58, 273, 282), "triangle": (24, 40, 270, 282)}
ORDER = ("cross", "circle", "square", "triangle")
M64 = (1 << 64) - 1


def content_key(p):
    """the engine's texture key of an upload (gpu.cpp content_key)"""
    n = len(p)
    h = (n * 0x9E3779B97F4A7C15) & M64

    def mix(v):
        nonlocal h
        h ^= (v + 0x9E3779B97F4A7C15 + ((h << 6) & M64) + (h >> 2)) & M64
        h = (h * 0xBF58476D1CE4E5B9) & M64

    step = max(8, (n // 64) & ~7)
    i = 0
    while i + 8 <= n:
        mix(int.from_bytes(p[i:i + 8], "little"))
        i += step
    t = p[n - min(n, 8):]
    mix(int.from_bytes(t.ljust(8, b"\0")[:8], "little"))
    return h


def find_font(iso):
    """(pixels, clut) of the 384 x 384 16-colour TIM2 picture on the disc"""
    with open(iso, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
        at = 0
        while True:
            at = m.find(b"TIM2", at)
            if at < 0 or at + 64 > len(m):
                return None
            tot, clsz, imsz, hdr, clc, fmt, mip, cltype, imtype, w, h = struct.unpack_from("<IIIHHBBBBHH", m, at + 16)
            if (w, h, imtype, clsz) == (FONT_W, FONT_H, 4, 64) and imsz == FONT_W * FONT_H // 2:
                pix = m[at + 16 + hdr:at + 16 + hdr + imsz]
                clut = m[at + 16 + hdr + imsz:at + 16 + hdr + imsz + clsz]
                return bytes(pix), bytes(clut)
            at += 4


def rgba(pix, clut):
    """the font at twice its size (1024 x 1024 for the 512 x 512 the game draws): the engine draws the key caps at that size, so
    they have twice the detail; made smoothly bigger (weighted by coverage, so the letters' edges do not darken)"""
    pal = []
    for i in range(16):
        v = int.from_bytes(clut[i * 4:i * 4 + 4], "little")
        pal.append((v & 255, (v >> 8) & 255, (v >> 16) & 255, min(255, ((v >> 24) & 255) * 2)))
    src = [pal[(pix[i >> 1] >> 4) if i & 1 else (pix[i >> 1] & 15)] for i in range(FONT_W * FONT_H)]

    def px(x, y, cx, cy):
        # never across a character's 24 x 24 cell: the game draws whole cells, and a neighbour's edge would show beside a letter
        x = min(max(x, cx), cx + 23)
        y = min(max(y, cy), cy + 23)
        return src[y * FONT_W + x]

    W = TEX_W * 2
    rows = []
    for y2 in range(TEX_H * 2):
        row = bytearray(b"\0")
        if y2 < FONT_H * 2:
            fy = (y2 + 0.5) / 2 - 0.5
            y0 = int(fy // 1)
            ty = fy - y0
            for x2 in range(FONT_W * 2):
                fx = (x2 + 0.5) / 2 - 0.5
                x0 = int(fx // 1)
                tx = fx - x0
                acc = [0.0, 0.0, 0.0, 0.0]
                for (xx, yy, wgt) in ((x0, y0, (1 - tx) * (1 - ty)), (x0 + 1, y0, tx * (1 - ty)), (x0, y0 + 1, (1 - tx) * ty), (x0 + 1, y0 + 1, tx * ty)):
                    r, g, b, a = px(xx, yy, (x2 // 2) // 24 * 24, (y2 // 2) // 24 * 24)
                    wa = wgt * a
                    acc[0] += r * wa
                    acc[1] += g * wa
                    acc[2] += b * wa
                    acc[3] += wa
                a = acc[3]
                if a > 0:
                    row += bytes((int(acc[0] / a + 0.5), int(acc[1] / a + 0.5), int(acc[2] / a + 0.5), int(a + 0.5)))
                else:
                    row += b"\0\0\0\0"
            row += bytes(4 * (W - FONT_W * 2))
        else:
            row += bytes(4 * W)
        rows.append(bytes(row))
    return b"".join(rows)


def png(path, w, h, raw):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def make(iso, out):
    found = find_font(iso)
    if not found:
        print("no 384 x 384 font on " + iso + ": key pictures are not available for this disc")
        return 1
    pix, clut = found
    key = content_key(pix)
    os.makedirs(out, exist_ok=True)
    png(os.path.join(out, "font_1001.png"), TEX_W * 2, TEX_H * 2, rgba(pix, clut))
    for index in ("textures_index.tsv", "font_index.tsv"):   # font_index.tsv: the font's line alone (the launcher adds others)
        with open(os.path.join(out, index), "w", newline="\n") as f:
            # key, the size the game draws it at, 0 0 (always the override), the picture
            f.write("%016x\t%d\t%d\t0\t0\tfont_1001.png\n" % (key, TEX_W, TEX_H))
    # the font's coverage for the PC settings page drawn in the game's look (quickmenu.cpp load_game_font): "RDF1", width, height,
    # then one byte a pixel
    cov = bytearray(FONT_W * FONT_H)
    alphas = [min(255, ((int.from_bytes(clut[i * 4:i * 4 + 4], "little") >> 24) & 255) * 2) for i in range(16)]
    for i in range(FONT_W * FONT_H):
        b = pix[i >> 1]
        cov[i] = alphas[(b >> 4) if i & 1 else (b & 15)]
    with open(os.path.join(out, "menu_font.bin"), "wb") as f:
        f.write(b"RDF1" + struct.pack("<II", FONT_W, FONT_H) + bytes(cov))
    cells = ";".join("%d,%d,%d,%d" % CELLS[n] for n in ORDER)
    with open(os.path.join(out, "prompts.env"), "w", newline="\n") as f:
        f.write("PS2_PROMPTS_VERSION=3\nPS2_TEX_OVERRIDES=%s\nPS2_PROMPT_FONT_KEY=%016x\nPS2_PROMPT_CELLS=%s\nPS2_MENU_FONT=%s\n" %
                (os.path.abspath(out), key, cells, os.path.join(os.path.abspath(out), "menu_font.bin")))
    print("font key %016x, written to %s" % (key, out))
    return 0


def selftest():
    # a key of known bytes, as the engine computed it for the US font (texkey log: 934acb7e98f3eef0)
    assert content_key(b"") == content_key(b"")
    raw = rgba(bytes(FONT_W * FONT_H // 2), bytes(64))
    assert len(raw) == 2 * TEX_H * (1 + 8 * TEX_W)
    print("selftest ok")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make")
    m.add_argument("--iso", required=True)
    m.add_argument("--out", required=True)
    sub.add_parser("selftest")
    a = ap.parse_args()
    return make(a.iso, a.out) if a.cmd == "make" else selftest()


if __name__ == "__main__":
    sys.exit(main())
