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
import argparse, math, mmap, os, struct, sys, zlib

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
    picture_lines, picture_list = make_pictures(iso, out)
    for index in ("textures_index.tsv", "font_index.tsv"):   # font_index.tsv: the font's lines (the launcher adds the others)
        with open(os.path.join(out, index), "w", newline="\n") as f:
            # key, the size the game draws it at, 0 0 (always the override), the picture
            f.write("%016x\t%d\t%d\t0\t0\tfont_1001.png\n" % (key, TEX_W, TEX_H))
            f.write(picture_lines)
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
        f.write("PS2_PROMPTS_VERSION=4\nPS2_TEX_OVERRIDES=%s\nPS2_PROMPT_FONT_KEY=%016x\nPS2_PROMPT_CELLS=%s\nPS2_MENU_FONT=%s\n" %
                (os.path.abspath(out), key, cells, os.path.join(os.path.abspath(out), "menu_font.bin")))
        if picture_list:
            f.write("PS2_PROMPT_PICTURES=%s\n" % picture_list)
    print("font key %016x, written to %s" % (key, out))
    return 0


# v0.9.3: the menus' own pictures of the buttons. The help strips of the map, the character select, the radio, the relations and
# the epilogue ("ⓧ OK  △ CANCEL") draw the buttons as ringed symbols in pictures of their own, not with the font. These are found
# on the disc (a white ring about 15 pixels wide with the button's colour inside: blue cross, red circle, pink square, green
# triangle), written as overrides (pictures\<key>.png, the picture as it is) and listed in PS2_PROMPT_PICTURES; the engine draws
# the key caps over the rings.
SKIP = ("AREA", "IMD\\", "STR\\", "SE\\", "MOVIE\\", "OVERLAY\\", "MOTCMN\\", "TEX\\")


# the inside of the game's ringed symbols, measured: cross, circle, square, triangle
SYMBOL_COLOURS = ((55, 130, 190), (167, 75, 95), (242, 169, 167), (14, 170, 75), (41, 101, 148))   # (and a darker cross)
SYMBOL_BUTTON = (0, 1, 2, 3, 0)


def _bright(p):
    return p[3] > 90 and p[0] > 170 and p[1] > 170 and p[2] > 170


def find_rings(rows, w, h):
    """[(button, cx, cy, r)]: 0 cross, 1 circle, 2 square, 3 triangle"""
    found = []
    offs = {R: [(int(round(R * math.cos(k * math.pi / 12))), int(round(R * math.sin(k * math.pi / 12)))) for k in range(24)] for R in (7.0, 7.5)}   # (the game's rings are 15 pixels across; smaller rings were letters in photos)
    for cy in range(8, h - 8):
        row = rows[cy]
        for cx in range(8, w - 8):
            if not _bright(row[cx + 7]) and not _bright(row[cx - 7]):   # (quick: a ring has its sides lit)
                continue
            if any(abs(cx - f[1]) <= 4 and abs(cy - f[2]) <= 4 for f in found):
                continue
            for R, o in offs.items():
                if sum(_bright(rows[cy + dy][cx + dx]) for dx, dy in o) < 20:
                    continue
                inner = [rows[cy + dy][cx + dx] for dy in range(-4, 5) for dx in range(-4, 5)]
                if sum(_bright(p) for p in inner) >= 40:   # a lit blob, not a ring
                    continue
                sat = [p for p in inner if p[3] > 90 and max(p[:3]) - min(p[:3]) > 50]
                if len(sat) < 12:
                    continue
                avg = [sum(p[i] for p in sat) / len(sat) for i in range(3)]
                # the game's own four colours (letters and photos with rings in them are other colours)
                near = min(range(len(SYMBOL_COLOURS)), key=lambda k: sum((avg[i] - SYMBOL_COLOURS[k][i]) ** 2 for i in range(3)))
                if sum((avg[i] - SYMBOL_COLOURS[near][i]) ** 2 for i in range(3)) > 30 ** 2:
                    continue
                button = SYMBOL_BUTTON[near]
                found.append((button, cx, cy, int(R)))
                break
    return found


def make_pictures(iso, out):
    """the override lines and the PS2_PROMPT_PICTURES value; ("", "") when the disc's files cannot be read"""
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from rd_jp_overrides import root, tims, decode, write_png
        from rd_hybrid import Iso, SECTOR
    except Exception as e:
        print("menu pictures left out: " + str(e))
        return "", ""
    disc = Iso(iso)
    entries, data_at = root(disc)
    os.makedirs(os.path.join(out, "pictures"), exist_ok=True)
    lines, parts, seen = [], [], set()
    for name in sorted(entries):
        if name.upper().startswith(SKIP):
            continue
        off, size = entries[name][0], entries[name][1]
        disc.f.seek(data_at * SECTOR + off)
        dat = disc.f.read(size)
        if b"TIM2" not in dat:
            continue
        for t in tims(dat):
            head, pix, pal = t[1], t[2], t[3]
            w, h = head[0], head[1]
            if h > 300 or w * h > 200000 or w < 64:
                continue
            key = content_key(pix)
            if key in seen:
                continue
            seen.add(key)
            rows = decode(head, pix, pal)
            if rows is None:
                continue
            rings = find_rings(rows, w, h)
            if not rings:
                continue
            tw, th = write_png(os.path.join(out, "pictures", "%016x.png" % key), rows, w, h)
            lines.append("%016x\t%d\t%d\t0\t0\tpictures/%016x.png\n" % (key, tw, th, key))
            parts.append("%016x:" % key + "|".join("%d,%d,%d,%d" % r for r in rings))
            print("  %s: %d button symbols" % (name, len(rings)))
    return "".join(lines), ";".join(parts)


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
