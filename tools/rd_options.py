#!/usr/bin/env python3
"""
rd_options.py - the words of the PC rows of the game's own Options list, painted into the pictures the game draws that
screen from (texture overrides), so the game itself shows them.

  rd_options.py make --iso "Raw Danger (USA).iso" --out <game folder>\\prompts [--recomp <recomp.ini>]

The Options screen draws its words from a 504 x 272 sheet (texture 0x7469) and its background from a 640 x 448 picture
(texture 0x7468), both in OPTION\\OPTION*.DAT; their uploads are padded to 512 x 512 and 1024 x 512, and the new words go in
those padded parts: the sheet's rows 276-511, the background's columns 640-1023 and rows 448-511. The words are put together
from tools/options_art.bin (the letters, made by rd_options_art_source.py in the look of the game's own words): the list in
tools/options_words.txt and the Game options of recomp.ini. Writes:
  options/<key>.png       the two pictures with the words (and options/jp/<key>.png: the Japanese look's, when <out>\\jp has it)
  options_index.tsv       their lines for the engine's texture overrides (the launcher puts them first)
  options_art.txt         where each word is: name, picture (S sheet / B background), x, y, w, h (PS2_NATIVE_OPTIONS_ART)
"""
import argparse, os, struct, sys, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rd_jp_overrides import Iso, root, tims, decode, SECTOR   # noqa: E402
from rd_prompts import content_key, png                          # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
WHITE, DARK = (251, 251, 251), (17, 42, 59)
LABEL_ROOM = 205   # a label wider than this is written in the small face, then cut


def read_faces():
    d = open(os.path.join(HERE, "options_art.bin"), "rb").read()
    if d[:4] != b"RDOF":
        sys.exit("options_art.bin: not the letters file")
    n = struct.unpack_from("<I", d, 4)[0]
    p, faces = 8, {}
    for _ in range(n):
        name = chr(d[p]); size, count = struct.unpack_from("<BH", d, p + 1); p += 4
        glyphs = {}
        for _ in range(count):
            c, adv, w = struct.unpack_from("<BHB", d, p); p += 4
            glyphs[c] = (adv / 16.0, w, d[p:p + w * 24]); p += w * 24
        faces[name] = glyphs
    return faces


def measure(face, text):
    return sum(face.get(ord(ch) if ord(ch) < 256 else 63, face[63])[0] for ch in text)


def render(face, text, colour):
    w = int(measure(face, text) + 4)
    img = bytearray(w * 24 * 4)
    x = 1.0
    for ch in text:
        adv, gw, mask = face.get(ord(ch) if ord(ch) < 256 else 63, face[63])
        x0 = int(round(x))
        for yy in range(24):
            for xx in range(gw):
                a = mask[yy * gw + xx]
                if not a or x0 + xx >= w:
                    continue
                o = (yy * w + x0 + xx) * 4
                if a > img[o + 3]:
                    img[o:o + 4] = bytes((colour[0], colour[1], colour[2], a))
        x += adv
    return w, img


def words(recomp):
    out = []
    for line in open(os.path.join(HERE, "options_words.txt"), encoding="utf-8"):
        if line.startswith("#") or "\t" not in line:
            continue
        kind, text = line.rstrip("\n").split("\t", 1)
        out.append((kind, text))
    if recomp and os.path.exists(recomp):
        # the rows are the live sections only (built-in ones are not switched in the game), named by their label (the
        # engine asks for the label first, the section's name when the label has no picture)
        sections, cur = [], None
        for line in open(recomp, encoding="utf-8", errors="replace"):
            line = line.strip()
            if line.startswith("[") and line.endswith("]"):
                cur = {"name": line[1:-1], "label": "", "live": True}
                sections.append(cur)
            elif cur is not None and "=" in line:
                k, v = (t.strip() for t in line.split("=", 1))
                if k.lower() == "label": cur["label"] = v
                elif k.lower() == "live": cur["live"] = v != "0"
        for sec in sections:
            if sec["live"]:
                out.append(("L", sec["label"] or sec["name"]))
    seen, uniq = set(), []
    for k, t in out:
        if (k, t) not in seen:
            seen.add((k, t)); uniq.append((k, t))
    return uniq


def art(recomp):
    faces = read_faces()
    items = [("A:up", *render(faces["V"], "\x01", WHITE)), ("A:down", *render(faces["V"], "\x02", WHITE)),
             ("A:left", *render(faces["V"], "\x03", WHITE)), ("A:right", *render(faces["V"], "\x04", WHITE))]
    for kind, text in words(recomp):
        if kind == "L":
            face = faces["L"] if measure(faces["L"], text) <= LABEL_ROOM else faces["S"]
            shown = text
            while measure(face, shown) > LABEL_ROOM + 30 and len(shown) > 4:
                shown = shown[:-2] + "…"[:0] + "."
            items.append(("L:" + text, *render(face, shown, WHITE)))
        elif kind == "V":
            items.append(("D:" + text, *render(faces["V"], text, DARK)))
        elif kind == "B":
            items.append(("W:" + text, *render(faces["V"], text, WHITE)))
            items.append(("D:" + text, *render(faces["V"], text, DARK)))
    # the free parts of the two pictures, filled row by row
    regions = [("S", 0, 276, 512, 512), ("B", 640, 0, 1024, 512), ("B", 0, 448, 640, 512)]
    layout, r, x, y = [], 0, regions[0][1], regions[0][2]
    for name, w, img in items:
        while True:
            tex, x0, y0, x1, y1 = regions[r]
            if x + w > x1:
                x, y = x0, y + 25
            if y + 24 <= y1:
                break
            r += 1
            if r == len(regions):
                break
            x, y = regions[r][1], regions[r][2]
        if r == len(regions):   # full: the words left over are not painted (those rows show no name) and the rest still works
            left = [n for n, _, _ in items[items.index((name, w, img)):]]
            print("rd_options.py: no room left for " + str(len(left)) + " words: " + ", ".join(left), file=sys.stderr)
            break
        layout.append((name, tex, x, y, w, img))
        x += w + 1
    return layout


def read_png(path):
    """The RGBA pictures this project writes (8-bit, colour type 6, no interlace): (w, h, rows of w * 4 bytes), or None."""
    try:
        d = open(path, "rb").read()
    except OSError:
        return None
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    p, idat = 8, b""
    w = h = 0
    while p < len(d):
        n, kind = struct.unpack_from(">I4s", d, p)
        body = d[p + 8:p + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, inter = struct.unpack(">IIBBBBB", body)
            if depth != 8 or ctype != 6 or inter:
                return None
        elif kind == b"IDAT":
            idat += body
        p += 12 + n
    raw, rows, prev, stride = zlib.decompress(idat), [], bytearray(w * 4), w * 4
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - 4] if x >= 4 else 0
            b = prev[x]
            c = prev[x - 4] if x >= 4 else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, rows


def paint(canvas, W, layout, tex):
    for nm, t, x0, y0, w, img in layout:
        if t != tex:
            continue
        for yy in range(24):
            o = ((y0 + yy) * W + x0) * 4
            canvas[o:o + w * 4] = img[yy * w * 4:(yy + 1) * w * 4]


def write_canvas(path, canvas, W, H):
    raw = bytearray()
    for y in range(H):
        raw += b"\0" + canvas[y * W * 4:(y + 1) * W * 4]
    os.makedirs(os.path.dirname(path), exist_ok=True)
    png(path, W, H, bytes(raw))


def make(iso_path, out, recomp):
    layout = art(recomp)
    iso = Iso(iso_path)
    ent, dat = root(iso)
    os.makedirs(os.path.join(out, "options"), exist_ok=True)
    lines, seen = [], set()
    for name in sorted(n for n in ent if n.startswith("OPTION\\OPTION")):
        off, size = ent[name]
        iso.f.seek(dat * SECTOR + off)
        data = iso.f.read(size)
        for _, head, pix, pal in tims(data):
            kind = {(504, 272): ("S", 512, 512), (640, 448): ("B", 1024, 512)}.get(tuple(head[:2]))
            if not kind:
                continue
            key = content_key(pix)
            if key in seen:
                continue
            seen.add(key)
            tex, W, H = kind
            rows = decode(head, pix, pal)
            canvas = bytearray(W * H * 4)
            for y in range(head[1]):
                row = b"".join(bytes(c) for c in rows[y])
                canvas[y * W * 4:y * W * 4 + len(row)] = row
            paint(canvas, W, layout, tex)
            write_canvas(os.path.join(out, "options", "%016x.png" % key), canvas, W, H)
            lines.append("%016x\t%d\t%d\t0\t0\toptions/%016x.png\n" % (key, W, H, key))
            # the same picture of the Japanese look (Enhanced Edition: <out>\jp\<key>.png, rd_jp_overrides.py), with the words:
            # the engine takes the first picture of the look in force, so this line comes before the Japanese list's
            jp = read_png(os.path.join(out, "jp", "%016x.png" % key))
            if jp and jp[0] == W and jp[1] == H:
                canvas = bytearray(b"".join(jp[2]))
                paint(canvas, W, layout, tex)
                write_canvas(os.path.join(out, "options", "jp", "%016x.png" % key), canvas, W, H)
                lines.append("%016x\t%d\t%d\t0\t0\toptions/jp/%016x.png\n" % (key, W, H, key))
    with open(os.path.join(out, "options_index.tsv"), "w", newline="\n") as f:
        f.writelines(lines)
    with open(os.path.join(out, "options_art.txt"), "w", newline="\n", encoding="utf-8") as f:
        for nm, t, x0, y0, w, _ in layout:
            f.write("%s\t%s\t%d\t%d\t%d\t%d\n" % (nm, t, x0, y0, w, 24))
    print(f"{len(layout)} words in {len(lines)} Options pictures, written to {out}")
    return 0 if len(lines) >= 2 else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make")
    m.add_argument("--iso", required=True)
    m.add_argument("--out", required=True)
    m.add_argument("--recomp", default="")
    a = ap.parse_args()
    return make(a.iso, a.out, a.recomp)


if __name__ == "__main__":
    sys.exit(main())
