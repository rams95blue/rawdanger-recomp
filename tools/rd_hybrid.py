#!/usr/bin/env python3
"""
rd_hybrid.py - the Enhanced Edition's disc: the US disc with the Japanese voices, made from your two disc images.

  rd_hybrid.py --us "Raw Danger (USA).iso" --jp "Zettai Zetsumei Toshi 2 (Japan).iso" --out hybrid.iso [--mute-us-only]

The disc holds both voice sets in the same layout (see the comment in main()); hybrid.iso.layout says where.

Works on the images directly (nothing is extracted, neither image is changed). v4 layout:

  [the US image up to STR.DAT] [Japanese voice area = STR.DAT] [new STR.FAT] [ROOT.FAT, ROOT.DAT moved up] [English area]

- reads /STR/STR.FAT + STR.DAT (every voice line, indexed by name) from both images; the voice area is in the US index
  order, the JP recording wherever the same line exists on the JP disc, the 7 English-only lines kept (or silenced with
  --mute-us-only), and the JP-only lines (NPCs saying the protagonists' names, ...) after them;
- the English area has the same layout (line i at the same offset in both), read only through the engine's remap
  (recomp.ini [Voices] remap_off) when the player picks English voices;
- the ISO 9660 directory records of the moved files and the volume size are rewritten.

The voice index (STR.FAT): "FAT ", the line count, and at 0xF4/0xF8 the offsets of the entries and of the names; then
per line (offset in STR.DAT, size, name offset), then the names. v2/v3 wrote those two header words as zero: the US
program then found no voice line at all (English or Japanese) and waited for each one - the softlock of v0.6.1-0.6.3.

The voice files (.VOI) are 16-bit PCM after a 2048-byte header: channels, sample count, a rate flag (1 = 24000 Hz, the US
dialogue; 0 = 48000 Hz, the JP dialogue and some sound effects). The US program plays both rates (tested: a 48 kHz line
plays at the right speed, at the same moment as the English one), so the Japanese lines keep their original 48 kHz.
--to-24k converts them like v3 did (half-band low-pass, every second sample), for comparison only.

Textures: the US release redrew 151 textures (tools/jp_textures.txt): "faces" (the characters' faces, hair, skin and clothes
colours) and "signs" (the writing in the world: signs, packets, papers, the game-over screen). Both versions go on the disc
like the voices: ROOT.FAT points each of them to a slot after the last file of ROOT.DAT (the US texture, slot as big as the
bigger of the two), and the Japanese copies follow the English voice area in the same layout; the engine reads those instead
while the in-game option is on (recomp.ini remap_on). --jp-textures faces,signs (or none) only says which start switched on.
Maps, models and overlays stay American: they go with the US program's code.

Exit code 0 and a summary line on success. The first bytes of a few US/JP lines are printed, to compare the formats.
"""
from __future__ import annotations

import argparse
import os
import shutil
import struct
import sys

SECTOR = 2048
FAT_HDR = 0x100


class Iso:
    """The few ISO 9660 things needed: find a file's directory record (where it sits, its sector and size)."""

    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        pvd = self.read(16, 1)
        if pvd[1:6] != b"CD001" or pvd[0] != 1:
            sys.exit(f"{path}: not a 2048-byte ISO 9660 image (a .bin/.cue disc image is not supported here)")
        self.root = pvd[156:156 + 34]

    def read(self, lba, count):
        self.f.seek(lba * SECTOR)
        return self.f.read(count * SECTOR)

    def records(self, lba, size):
        """(name, extent, size, flags, absolute byte position of the record) for each entry of a directory."""
        data = self.read(lba, (size + SECTOR - 1) // SECTOR)
        pos = 0
        while pos < len(data):
            n = data[pos]
            if n == 0:
                pos = (pos // SECTOR + 1) * SECTOR  # records do not cross sectors: the rest of this one is padding
                continue
            rec = data[pos:pos + n]
            extent, length = struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
            name_len = rec[32]
            name = rec[33:33 + name_len].decode("ascii", "replace").split(";")[0]
            yield name, extent, length, rec[25], lba * SECTOR + pos
            pos += n

    def find(self, path):
        extent, size = struct.unpack_from("<I", self.root, 2)[0], struct.unpack_from("<I", self.root, 10)[0]
        parts = [p for p in path.strip("/").split("/") if p]
        found = None
        for i, part in enumerate(parts):
            found = None
            for name, ext, length, flags, at in self.records(extent, size):
                if name.upper() == part.upper():
                    found = (ext, length, at)
                    break
            if not found:
                sys.exit(f"{self.path}: {path} not found")
            extent, size = found[0], found[1]
        return found  # (extent, size, record position)

    def files(self):
        """every file of the image: (path, extent, size, absolute byte position of its directory record)"""
        out = []

        def walk(extent, size, path, depth):
            for name, ext, length, flags, at in self.records(extent, size):
                if name in ("\x00", "\x01") or depth > 8:
                    continue
                if flags & 2:
                    walk(ext, length, path + name + "/", depth + 1)
                else:
                    out.append((path + name, ext, length, at))

        walk(struct.unpack_from("<I", self.root, 2)[0], struct.unpack_from("<I", self.root, 10)[0], "/", 0)
        return out

    def bytes(self, extent, size):
        self.f.seek(extent * SECTOR)
        return self.f.read(size)


def read_fat(data, where):
    if data[:4] != b"FAT ":
        sys.exit(f"{where}: STR.FAT is not a FAT index")
    n = struct.unpack_from("<I", data, 4)[0]
    out = []
    for i in range(n):
        off, size, no = struct.unpack_from("<III", data, FAT_HDR + i * 12)
        out.append((data[no:data.index(b"\0", no)].decode("ascii"), off, size))
    return out


def make_fat(entries):
    n = len(entries)
    names = bytearray()
    offs = []
    base = FAT_HDR + n * 12
    for name, _, _ in entries:
        offs.append(base + len(names))
        names += name.encode("ascii") + b"\0"
    out = bytearray(b"FAT " + struct.pack("<I", n)).ljust(FAT_HDR, b"\0")
    # the header's last two words: where the entries start and where the names start. The game reads both; a FAT with
    # them zero (rd_hybrid v2/v3) makes the US program find no voice line at all, English or Japanese, and wait forever.
    struct.pack_into("<II", out, FAT_HDR - 12, FAT_HDR, base)
    for (name, off, size), no in zip(entries, offs):
        out += struct.pack("<III", off, size, no)
    return bytes(out + names)


VOI_HEADER = 2048


def voi_info(head):
    """(channels, samples, rate flag) of a .VOI header, or None"""
    if len(head) < 12:
        return None
    ch, n, flag = struct.unpack_from("<III", head, 0)
    return (ch, n, flag) if ch in (1, 2) and flag in (0, 1) else None


def converted_size(info):
    """the byte size of a 48 kHz line once made 24 kHz, or None when it is not converted"""
    ch, n, flag = info
    if flag != 0:
        return None
    if ch == 1:
        return VOI_HEADER + ((n + 1) // 2) * 2
    per = (n // 2 + 255) // 256 * 256          # samples per channel, whole 256-sample blocks at 48 kHz
    return VOI_HEADER + (per // 2) * 2 * 2       # 128-sample blocks per channel at 24 kHz


def _half(x):
    """48 kHz -> 24 kHz: y[k] = (x[2k-1] + 2 x[2k] + x[2k+1]) / 4 (a small half-band low-pass, then every second sample)"""
    import array
    even = x[0::2]
    odd = x[1::2]
    prev = array.array("h", [0]) + odd[:len(even) - 1]
    nxt = odd + array.array("h", [0] * (len(even) - len(odd)))
    return array.array("h", [(a + 2 * b + c) >> 2 for a, b, c in zip(prev, even, nxt)])


def to_24k(data):
    """a 48 kHz .VOI made 24 kHz, in the layout the US program plays: mono as it is; stereo in blocks of 128 samples per
    channel (48 kHz stereo is in blocks of 256). Header: same channels, the new sample count, rate flag 1."""
    import array
    ch, n, flag = struct.unpack_from("<III", data, 0)
    x = array.array("h")
    x.frombytes(data[VOI_HEADER:VOI_HEADER + n * 2])
    if sys.byteorder != "little":
        x.byteswap()
    if ch == 1:
        y = _half(x)
    else:
        if len(x) % 512:
            x.extend([0] * (512 - len(x) % 512))
        left, right = array.array("h"), array.array("h")
        for b in range(0, len(x), 512):
            left.extend(x[b:b + 256])
            right.extend(x[b + 256:b + 512])
        left, right = _half(left), _half(right)
        y = array.array("h")
        for b in range(0, len(left), 128):
            y.extend(left[b:b + 128])
            y.extend(right[b:b + 128])
    if sys.byteorder != "little":
        y.byteswap()
    head = bytearray(data[:VOI_HEADER])
    struct.pack_into("<III", head, 0, ch, len(y), 1)
    return bytes(head) + y.tobytes()


def with_tims(usd, jpd, which, name):
    """The US file with the TIM2 pictures numbered in `which` taken from the Japanese file (same place and size in both: the logos
    in BOOT.DAT and TITLE.DAT), or the US file unchanged when they are not where expected."""
    def tims(d):
        out, p = [], 0
        while True:
            p = d.find(b"TIM2", p)
            if p < 0: return out
            out.append(p); p += 4
    tu, tj = tims(usd), tims(jpd)
    out = bytearray(usd)
    if which is None:   # every picture: the textures (v0.8.3)
        if len(tu) != len(tj):
            print(f"  {name}: not the same pictures in both, kept as in the US release")
            return bytes(out)
        which = list(range(len(tu)))
    tu, tj = tu + [len(usd)], tj + [len(jpd)]
    for k in which:
        if k + 1 >= len(tu) or k + 1 >= len(tj) or tu[k + 1] - tu[k] != tj[k + 1] - tj[k]:
            print(f"  {name}: picture {k} is not in the same place in both, kept as in the US release")
            continue
        # the picture and its palette only: the header (where the game puts it in the GS's memory) stays the US one, since the
        # Japanese title keeps its logo elsewhere and there it overwrote the US menu's words (START GAME with Japanese letters)
        q = tu[k] + (16 if usd[tu[k] + 5] == 0 else 128)
        hdr = struct.unpack_from("<H", usd, q + 12)[0]
        qj0 = tj[k] + (q - tu[k])
        if (struct.unpack_from("<III", usd, q) != struct.unpack_from("<III", jpd, qj0) or usd[q + 17:q + 24] != jpd[qj0 + 17:qj0 + 24]
                or usd[q + 12:q + 14] != jpd[qj0 + 12:qj0 + 14]):
            print(f"  {name}: picture {k} differs in size or format, kept as in the US release")
            continue
        qj = tj[k] + (q - tu[k])
        out[q + hdr:tu[k + 1]] = jpd[qj + hdr:tj[k + 1]]
    return bytes(out)


# ---- "ENHANCED EDITION" and the build number on the title screen (v0.8.6) ----

def load_title_art(path):
    d = open(path, "rb").read()
    if d[:4] != b"RDTA":
        return None
    n = struct.unpack_from("<H", d, 6)[0]
    p, art = 8, {}
    for _ in range(n):
        ch, size, w, h = struct.unpack_from("<BBBB", d, p); p += 4
        stride = (w + 7) // 8
        rows = []
        for y in range(h):
            row = d[p + y * stride:p + (y + 1) * stride]
            rows.append([(row[x // 8] >> (7 - x % 8)) & 1 for x in range(w)])
        p += stride * h
        art[(chr(ch), size)] = rows
    return art


# The title words' canvas for art made by hand (tools/title_art_us.png, title_art_jp.png: RGBA PNG, the words only, on a clear
# background): the title picture's corner from x 300, y 330 to its edge (340 x 118) at 8 times the size, 2720 x 944. The PSD that
# tools/rd_title_art_source.py makes has that canvas, with the logo as a guide layer. The build number is still drawn here.
TITLE_ART_AT, TITLE_ART_SCALE = (300, 330), 8
JP_WORDS_LEFT = 383   # the Japanese words' left end (the logo starts at 381; the US menu's boxes at 507)


def read_rgba_png(path):
    try:
        from rd_options import read_png   # (pure Python: the embedded Python has no Pillow)
    except Exception:
        return None
    got = read_png(path)
    return got if got and got[0] == 340 * TITLE_ART_SCALE and got[1] == 118 * TITLE_ART_SCALE else None


def title_words(dat, version, art_path):
    """TITLE.DAT with "ENHANCED EDITION" and the build number by the title logo (picture 8, 640 x 448, 8 bits a pixel with a
    256-colour palette, drawn over the whole title screen), in that logo's look: under the US RAW DANGER logo in its own yellow
    (copied from inside its letters, so it has the same grain) with its red rim and a dark edge inside, leaning like it; over the
    Japanese logo (white on the blue title, with a soft dark edge) in its white and grey. Every colour is one of the picture's
    palette; nothing changes when the picture is not one of the two."""
    art = load_title_art(art_path) if os.path.exists(art_path) else None
    if art is None:
        return dat
    tims, p = [], 0
    while True:
        p = dat.find(b"TIM2", p)
        if p < 0: break
        tims.append(p); p += 4
    if len(tims) < 9:
        return dat
    t = tims[8]
    q = t + (16 if dat[t + 5] == 0 else 128)
    total, clut, img, hsz, ncol, fmt, mip, ctype, itype, w, h = struct.unpack_from("<IIIHHBBBBHH", dat, q)
    if (w, h, itype, ctype & 0x1F, ncol) != (640, 448, 5, 3, 256):
        return dat
    pix, pal_at = q + hsz, q + hsz + img
    raw = [tuple(dat[pal_at + 4 * i:pal_at + 4 * i + 4]) for i in range(256)]
    # CSM1 order: the index in the pixels picks entry i with bits 3 and 4 swapped
    def entry(i): return raw[(i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)]
    colours = [entry(i) for i in range(256)]
    out = bytearray(dat)
    W, H = w, h
    at = lambda x, y: out[pix + y * W + x]

    def nearest(rgba, solid=True):
        best, bi = None, 0
        for i, (r, g, b, a) in enumerate(colours):
            if solid and a < 0x70: continue
            dd = (r - rgba[0]) ** 2 + (g - rgba[1]) ** 2 + (b - rgba[2]) ** 2 + (0 if solid else 4 * (a - rgba[3]) ** 2)
            if best is None or dd < best: best, bi = dd, i
        return bi

    # which logo: the US one's yellow (x 368..621, y 363..420), or the Japanese one's white (x 378..621, y 362..435)
    yellow, white = [], 0
    for y in range(362, 436):
        for x in range(368, 622):
            r, g, b, a = colours[at(x, y)]
            if a >= 0x70 and r > 200 and g > 190 and b < 90 and y <= 420: yellow.append(at(x, y))
            if a >= 0x70 and r > 240 and g > 240 and b > 240: white += 1
    if len(yellow) >= 200:
        style = "us"
        fill = lambda k: yellow[(k * 7919) % len(yellow)]
        rim, edge = nearest((236, 0, 0, 0x80)), nearest((96, 82, 0, 0x80))
        right, base, lean, rim_w = 618, 441, 40, 2
    elif white >= 500:
        style = "jp"
        solid_white = nearest((255, 255, 255, 0x80))
        fill = lambda k: solid_white
        rim, edge = nearest((24, 24, 32, 0x30), solid=False), nearest((200, 204, 214, 0x80))
        right, base, lean, rim_w = 618, 360, 0, 1
    else:
        return dat

    def line(text, size):
        glyphs = [art.get((c, size)) or art.get((c.upper(), size)) for c in text]
        if any(g is None for g in glyphs): return None
        gh = len(glyphs[0]); width = sum(len(g[0]) for g in glyphs) + (len(glyphs) - 1)
        mask = [[0] * width for _ in range(gh)]
        x0 = 0
        for g in glyphs:
            for y in range(gh):
                for x, bit in enumerate(g[y]):
                    if bit: mask[y][x0 + x] = 1
            x0 += len(g[0]) + 1
        return mask

    build = line("v" + version, 1) if version else None
    # art made by hand for this logo: its pixels, made small (8 x 8 to one), each the nearest colour of the palette
    hand = read_rgba_png(os.path.join(os.path.dirname(os.path.abspath(__file__)), "title_art_" + style + ".png"))
    if hand:
        hw, hh, rows = hand
        S = TITLE_ART_SCALE
        cells, x_max = {}, -1
        for cy in range(hh // S):
            for cx in range(hw // S):
                tr = tg = tb = ta = 0
                for yy in range(S):
                    row = rows[cy * S + yy]
                    for xx in range(S):
                        o = (cx * S + xx) * 4
                        a = row[o + 3]
                        tr += row[o] * a; tg += row[o + 1] * a; tb += row[o + 2] * a; ta += a
                if ta >= 128 * S * S:   # mostly covered
                    cells[(cx, cy)] = nearest((tr // ta, tg // ta, tb // ta, 0x80))
                    x_max = max(x_max, cx)
        room = (W - 3) - (TITLE_ART_AT[0] + x_max) - (len(build[0]) + 6 if build else 0)
        shift = min(0, room)   # moved left when the build number would not fit after it
        if style == "jp":
            # v0.9.1: over the Japanese logo the US program's title menu (its boxes from x 507, OPTION at y 336-347) covered the
            # words' right end; they start where the logo starts (x 381) instead, and end before the menu
            x_min = min(cx for cx, cy in cells)
            shift = JP_WORDS_LEFT - (TITLE_ART_AT[0] + x_min)
        for (cx, cy), idx in cells.items():
            X, Y = TITLE_ART_AT[0] + cx + shift, TITLE_ART_AT[1] + cy
            if 0 <= X < W and 0 <= Y < H: out[pix + Y * W + X] = idx
        print("  the title words from tools/title_art_" + style + ".png")
        if not build:
            return bytes(out)
        # the number sits on the words' line where they end (their last 12 columns: the US words lean, so their right end is
        # higher than their left), and leans with them
        bottom = max((cy for cx, cy in cells if cx >= x_max - 12), default=base - TITLE_ART_AT[1])
        parts, x, base = [build], TITLE_ART_AT[0] + x_max + shift + 6, TITLE_ART_AT[1] + bottom + 1 - rim_w   # (on the letters, not their rim)
        start_x = x
    else:
        words = line("ENHANCED EDITION", 0)
        if words is None:
            return dat
        parts = [words] + ([build] if build else [])
        total_w = sum(len(m[0]) for m in parts) + 6 * (len(parts) - 1)
        x = start_x = right - total_w
    k = 0
    for m in parts:
        mh, mw = len(m), len(m[0])
        top = base - mh
        def on(xx, yy): return 0 <= yy < mh and 0 <= xx < mw and m[yy][xx]
        for yy in range(-rim_w, mh + rim_w):
            for xx in range(-rim_w, mw + rim_w):
                X = x + xx; Y = top + yy - ((X - start_x) // lean if lean else 0)
                if not (0 <= X < W and 0 <= Y < H): continue
                if on(xx, yy):
                    inner_edge = not on(xx + 1, yy) or not on(xx, yy + 1)
                    out[pix + Y * W + X] = edge if inner_edge else fill(k)
                    k += 1
                elif any(on(xx + dx, yy + dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1)) or \
                        (rim_w >= 2 and any(on(xx + dx, yy + dy) for dx, dy in ((2, 0), (-2, 0), (0, 2), (0, -2)))):
                    out[pix + Y * W + X] = rim
        x += mw + 6
    return bytes(out)


def plan_textures(us, jp):
    """The textures with a US and a Japanese version (tools/jp_textures.txt), as {"fat": the new ROOT.FAT, "groups": [{"name",
    "count", "items": [(name, offset in the new ROOT.DAT, slot bytes, US data, JP data)]}]}, or None when the list is missing.
    ROOT.FAT is STR.FAT's format with absolute name offsets: "FAT ", count, at 0xF4/0xF8 where the entries and the names
    start; each entry (offset in ROOT.DAT, size, name offset). Each texture's entry points to its new slot, with the size of the
    bigger version (the game reads that many bytes; the TIM2 header says how much of it is the picture)."""
    listing = os.path.join(os.path.dirname(os.path.abspath(__file__)), "jp_textures.txt")
    if not os.path.exists(listing):
        print("no tools/jp_textures.txt: the Japanese textures are left out")
        return None
    names, alias, only_tims = {}, {}, {}
    for line in open(listing, encoding="utf-8"):
        line = line.split("#", 1)[0].strip()
        if line:
            parts = line.split("\t")
            name, group = parts[0], parts[1]
            key = name.replace("/", "\\").upper()
            names[key] = group.strip().lower()
            if len(parts) > 2 and parts[2].strip() not in ("", "-"):   # a US-only copy of a redrawn texture: its Japanese version is the other name's
                alias[key] = parts[2].strip().replace("/", "\\").upper()
            if len(parts) > 3 and parts[3].strip().startswith("tims="):   # the US file with only these pictures (TIM2, counted from 0) from the Japanese one
                only_tims[key] = [int(x) for x in parts[3].strip()[5:].split(",") if x]

    def root(iso):
        fe, fs, _ = iso.find("/ROOT.FAT")
        de, ds, _ = iso.find("/ROOT.DAT")
        fat = bytearray(iso.bytes(fe, fs))
        if fat[:4] != b"FAT ":
            sys.exit(f"{iso.path}: ROOT.FAT is not a FAT index")
        n = struct.unpack_from("<I", fat, 4)[0]
        first, names_at = struct.unpack_from("<II", fat, FAT_HDR - 12)
        entries = {}
        for i in range(n):
            off, size, no = struct.unpack_from("<III", fat, first + 12 * i)
            at = no if no >= names_at else names_at + no
            entries[fat[at:fat.index(b"\0", at)].decode("latin-1").upper()] = (i, off, size)
        return fat, first, entries, de, ds

    ufat, ufirst, uent, ude, uds = root(us)
    _, _, jent, jde, _ = root(jp)
    pos = (uds + SECTOR - 1) // SECTOR * SECTOR
    groups = []
    for group in sorted(set(names.values())):
        items = []
        for name in sorted(n for n, g in names.items() if g == group):
            jname = alias.get(name, name)
            if name not in uent or jname not in jent:
                continue
            i, off, size = uent[name]
            _, joff, jsize = jent[jname]
            us.f.seek(ude * SECTOR + off); usd = us.f.read(size)
            jp.f.seek(jde * SECTOR + joff); jpd = jp.f.read(jsize)
            if name in only_tims:
                jpd = with_tims(usd, jpd, only_tims[name], name)
                jsize = len(jpd)
            elif names[name] != "movies" and usd[:4] == b"TIM2":
                # v0.8.3: the Japanese pictures in the US files: each picture keeps the US header (where it goes in the GS's
                # memory and how wide the game takes it: the Japanese files say otherwise for 98 of them, and the US models then
                # showed the wrong part of the texture: the cook boy's and the waitress' clothes), and a picture of another size
                # or format stays the US one
                jpd = with_tims(usd, jpd, None, name)
                jsize = len(jpd)
            slot = (max(size, jsize) + SECTOR - 1) // SECTOR * SECTOR
            struct.pack_into("<II", ufat, ufirst + 12 * i, pos, max(size, jsize))
            items.append((name, pos, slot, usd, jpd))
            pos += slot
        groups.append({"name": group, "count": len(items), "items": items})
    print("textures with a Japanese version: " + ", ".join(f"{g['name']} {g['count']}" for g in groups))
    # The sound banks with voices in them (the player characters' shouts, wake-up and recorder lines, and four scenes' banks:
    # SE\PLxx*.HD/BD/SQ, SE\1-5-1, 4-2-0P0, 4-2-0P3, 4-3-0): recorded again in English for the US release, so the Enhanced
    # Edition shouted in English. Same scheme as the textures, the other way round: ROOT.FAT points to the Japanese bank (what
    # the game reads with Japanese voices on), the English copies follow in the same layout and the voice switch reads those
    # (recomp.ini [Voices] remap2_off). A bank's HD, BD and SQ always come from the same version.
    items = []
    for name in sorted(n for n in uent if n.startswith("SE\\") and n in jent):
        i, off, size = uent[name]
        _, joff, jsize = jent[name]
        us.f.seek(ude * SECTOR + off); usd = us.f.read(size)
        jp.f.seek(jde * SECTOR + joff); jpd = jp.f.read(jsize)
        if usd == jpd:
            continue
        slot = (max(size, jsize) + SECTOR - 1) // SECTOR * SECTOR
        # the size: the bigger of the two (a bank's header says how much of it is used; the rest reads as silence)
        struct.pack_into("<II", ufat, ufirst + 12 * i, pos, max(size, jsize))
        items.append((name, pos, slot, jpd, usd))   # (the slot holds the Japanese bank, the copy the English one)
        pos += slot
    groups.append({"name": "sounds", "count": len(items), "items": items})
    print(f"sound banks with Japanese voices: {len(items)} files")
    return {"fat": bytes(ufat), "groups": groups, "count": sum(g["count"] for g in groups)}


def both_endian32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--us", required=True)
    ap.add_argument("--jp", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--mute-us-only", action="store_true")
    ap.add_argument("--to-24k", action="store_true", help="make the Japanese 48 kHz lines 24 kHz like the English ones (not needed: the "
                    "US program plays 48 kHz lines; kept for comparison)")
    ap.add_argument("--no-convert", action="store_true", help=argparse.SUPPRESS)   # the default since v4
    ap.add_argument("--version", default="", help="the build number written under the title screen's logo with ENHANCED EDITION "
                    "(tools/title_art.bin); none: no words")
    ap.add_argument("--jp-textures", default="", help="texture groups (faces, signs) that start switched on in the game; both "
                    "groups are always on the disc")
    a = ap.parse_args(argv)

    us, jp = Iso(a.us), Iso(a.jp)
    us_fat_ext, us_fat_size, _ = us.find("/STR/STR.FAT")
    us_dat_ext, us_dat_size, _ = us.find("/STR/STR.DAT")
    jp_fat_ext, jp_fat_size, _ = jp.find("/STR/STR.FAT")
    jp_dat_ext, jp_dat_size, _ = jp.find("/STR/STR.DAT")
    us_fat = read_fat(us.bytes(us_fat_ext, us_fat_size), a.us)
    jp_fat = read_fat(jp.bytes(jp_fat_ext, jp_fat_size), a.jp)
    jp_by = {n.upper(): (o, s) for n, o, s in jp_fat}
    us_names = {n.upper() for n, _, _ in us_fat}
    print(f"US voices: {len(us_fat)} lines, JP voices: {len(jp_fat)} lines")

    plan = []
    for name, off, size in us_fat:
        j = jp_by.get(name.upper())
        if j: plan.append((name, "jp", j[0], j[1]))
        elif a.mute_us_only: plan.append((name, "mute", 0, 0))
        else: plan.append((name, "us", off, size))
    jp_only = [(n, "jp", o, s) for n, o, s in jp_fat if n.upper() not in us_names]
    plan += jp_only
    for name, off, size in [x for x in us_fat if x[0].upper() in jp_by][:3]:
        us.f.seek(us_dat_ext * SECTOR + off)
        jo = jp_by[name.upper()]
        jp.f.seek(jp_dat_ext * SECTOR + jo[0])
        print(f"  {name}: US {us.f.read(16).hex()} ({size:,} B)  JP {jp.f.read(16).hex()} ({jo[1]:,} B)")

    # Two voice areas with the same layout, one English and one Japanese: line i sits at the same offset in both (each slot
    # as big as the bigger of the two recordings), so the game finds every line through one STR.FAT and the engine can
    # switch the language while the game runs by reading the other area (recomp.ini: [Voices] remap). STR.DAT points to the
    # Japanese area; the English one follows it.
    # which Japanese lines must be made 24 kHz: the ones whose English line is 24 kHz (all the dialogue), and the Japanese-only
    # 48 kHz lines (the same kind of dialogue)
    us_head = {}
    for name, off, size in us_fat:
        us.f.seek(us_dat_ext * SECTOR + off)
        us_head[name.upper()] = voi_info(us.f.read(12))
    convert = {}   # name -> new size
    rates = {}
    for name, (off, size) in jp_by.items():
        jp.f.seek(jp_dat_ext * SECTOR + off)
        info = voi_info(jp.f.read(12))
        if info is None:
            continue
        rates[info[2]] = rates.get(info[2], 0) + 1
        u = us_head.get(name)
        if not a.to_24k or info[2] != 0 or (u is not None and u[2] != 1):
            continue
        new = converted_size(info)
        if new is not None:
            convert[name] = new
    print(f"Japanese lines: {rates.get(0, 0)} at 48 kHz, {rates.get(1, 0)} at 24 kHz; {len(convert)} made 24 kHz like the English ones"
          + ("" if a.to_24k else " (kept at their original 48 kHz)"))
    jp_size = {n: convert.get(n, s) for n, (o, s) in jp_by.items()}

    us_by = {n.upper(): (o, s) for n, o, s in us_fat}
    slots = []  # (name, us (off,size) or None, jp (off,size) or None, slot bytes)
    for name, src, off, size in plan:
        u = us_by.get(name.upper())
        j = jp_by.get(name.upper())
        if src == "mute": u = None
        biggest = max(u[1] if u else 0, jp_size[name.upper()] if j else 0)
        slots.append((name, u, j, (biggest + SECTOR - 1) // SECTOR * SECTOR))
    area = sum(x[3] for x in slots)

    # v4 layout: the voice area the game reads stays where STR.DAT always was (any place works, as long as STR.FAT's header
    # is right; low on the disc is simply the US layout), the files that came after it move up, and the English area is
    # last. The English area is only read through the engine's remap (recomp.ini [Voices] remap_off), which works anywhere.
    #   [the US image up to STR.DAT] [Japanese voice area = STR.DAT] [STR.FAT] [ROOT.FAT, ROOT.DAT, ...] [English voice area]
    swaps = plan_textures(us, jp)
    after = sorted((f for f in us.files() if f[1] > us_dat_ext and f[0].upper() not in ("/STR/STR.DAT", "/STR/STR.FAT")), key=lambda f: f[1])
    tmp = a.out + ".part"
    print(f"writing {a.out} ...", flush=True)
    moved = []
    with open(tmp, "wb") as out:
        us.f.seek(0)
        left = us_dat_ext * SECTOR
        while left:
            chunk = us.f.read(min(8 << 20, left))
            out.write(chunk)
            left -= len(chunk)
        jp_start = us_dat_ext

        def write_area(prefer_jp, label):
            done = 0
            for i, (name, u, j, slot) in enumerate(slots, 1):
                pick = (j, jp, jp_dat_ext) if (prefer_jp and j) or not u else (u, us, us_dat_ext)
                (off, size), iso, base = pick if pick[0] else ((0, 0), us, us_dat_ext)
                iso.f.seek(base * SECTOR + off)
                if iso is jp and name.upper() in convert:
                    line = to_24k(iso.f.read(size))
                    out.write(line)
                    size = len(line)
                else:
                    left = size
                    while left:
                        chunk = iso.f.read(min(8 << 20, left))
                        if not chunk: sys.exit(f"{name}: the source image ends early")
                        out.write(chunk)
                        left -= len(chunk)
                if slot > size: out.write(b"\0" * (slot - size))
                done += slot
                if i % 500 == 0 or i == len(slots):
                    print(f"  {label} voices {i}/{len(slots)}  {done / 1e6:,.0f} MB", flush=True)

        write_area(True, "Japanese")
        final, pos = [], 0
        for name, u, j, slot in slots:
            # the size: the bigger recording (each line's header says how many samples it has)
            final.append((name, pos, max(u[1] if u else 0, jp_size[name.upper()] if j else 0)))
            pos += slot
        fat = make_fat(final)
        fat_start = out.tell() // SECTOR
        out.write(fat)
        out.write(b"\0" * ((-out.tell()) % SECTOR))
        for path, ext, size, at in after:
            new = out.tell() // SECTOR
            if swaps and path.upper() == "/ROOT.FAT":
                out.write(swaps["fat"])
                out.write(b"\0" * ((-out.tell()) % SECTOR))
                moved.append((at, new, size))
                print(f"  {path}: sector {ext} -> {new} ({swaps['count']} Japanese textures and sound banks)", flush=True)
                continue
            us.f.seek(ext * SECTOR)
            left = size
            while left:
                chunk = us.f.read(min(8 << 20, left))
                if not chunk: sys.exit(f"{path}: the US image ends early")
                out.write(chunk)
                left -= len(chunk)
            if swaps and path.upper() == "/ROOT.DAT":
                # the US copies of the textures with two versions, group by group, each slot as big as the bigger one
                start = new * SECTOR
                out.write(b"\0" * ((-out.tell()) % SECTOR))
                for group in swaps["groups"]:
                    group["us_start"] = out.tell() // SECTOR
                    for n_item, (name, off, slot, usd, jpd) in enumerate(group["items"]):
                        assert out.tell() - start == off
                        if name.upper().endswith("TITLE\\TITLE.DAT") and a.version:
                            letters = os.path.join(os.path.dirname(os.path.abspath(__file__)), "title_art.bin")
                            stamped, stamped_jp = title_words(usd, a.version, letters), title_words(jpd, a.version, letters)
                            print(f"  {name}: ENHANCED EDITION v{a.version} by the US logo: {'yes' if stamped != usd else 'no'}, "
                                  f"by the Japanese logo: {'yes' if stamped_jp != jpd else 'no'}")
                            usd, jpd = stamped, stamped_jp
                            group["items"][n_item] = (name, off, slot, usd, jpd)   # (the Japanese copy is written later)
                        out.write(usd + b"\0" * (slot - len(usd)))
                size = out.tell() - start
            out.write(b"\0" * ((-out.tell()) % SECTOR))
            moved.append((at, new, size))
            print(f"  {path}: sector {ext} -> {new}", flush=True)
        en_start = out.tell() // SECTOR
        write_area(False, "English")
        if swaps:
            # the Japanese copies, in the same layout as the US ones (the engine reads these while the option is on)
            for group in swaps["groups"]:
                group["jp_start"] = out.tell() // SECTOR
                for name, off, slot, usd, jpd in group["items"]:
                    out.write(jpd + b"\0" * (slot - len(jpd)))
                group["sectors"] = out.tell() // SECTOR - group["jp_start"]
        total = out.tell() // SECTOR

    with open(tmp, "r+b") as out:
        o2 = Iso.__new__(Iso)
        o2.path, o2.f = tmp, out
        pvd = o2.read(16, 1)
        o2.root = pvd[156:156 + 34]
        for path, start, size in (("/STR/STR.DAT", jp_start, area), ("/STR/STR.FAT", fat_start, len(fat))):
            _, _, at = o2.find(path)
            moved.append((at, start, size))
        for at, start, size in moved:
            out.seek(at + 2); out.write(both_endian32(start))
            out.seek(at + 10); out.write(both_endian32(size))
        for lba in range(16, 32):
            d = o2.read(lba, 1)
            if d[1:6] != b"CD001" or d[0] == 255: break
            if d[0] in (1, 2):
                out.seek(lba * SECTOR + 80); out.write(both_endian32(total))
    os.replace(tmp, a.out)
    # where the two areas are, for the launcher's recomp.ini (the in-game voice switch)
    with open(a.out + ".layout", "w", encoding="ascii") as f:
        tex = "".join((f"sounds_jp_slots = {g['us_start']} {g['jp_start']} {g['sectors']} {g['count']}\n" if g["name"] == "sounds" else
                       f"textures_{g['name']} = {g['us_start']} {g['jp_start']} {g['sectors']} {g['count']}\n") for g in (swaps["groups"] if swaps else []))
        f.write(f"version = 4\nmedia = 2\ntitle_words = {a.version or '-'}\ntextures_start_on = {a.jp_textures}\n{tex}voices_jp_start = {jp_start}\nvoices_en_start = {en_start}\nvoices_sectors = {area // SECTOR}\n"
                f"voices_converted = {len(convert)}\n")
    print(f"voice areas: Japanese at sector {jp_start} (STR.DAT), English at {en_start}, {area // SECTOR} sectors each")
    print(f"hybrid disc: {a.out}  ({total * SECTOR / 1e9:.2f} GB)")

    counts = {k: sum(1 for p in plan if p[1] == k) for k in ("jp", "us", "mute")}
    print(f"voices: {counts['jp'] - len(jp_only)} Japanese in place of English, {len(jp_only)} Japanese-only added, "
          f"{counts['us']} English-only kept, {counts['mute']} silenced")
    return 0


if __name__ == "__main__":
    sys.exit(main())
