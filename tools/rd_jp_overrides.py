#!/usr/bin/env python3
"""rd_jp_overrides.py - the Japanese look for the pictures inside the game's other files (maps, characters, models).

  python tools/rd_jp_overrides.py make --us US.iso --jp JP.iso --out FOLDER
  python tools/rd_jp_overrides.py selftest

The Enhanced Edition's "Japanese faces" switch swaps the 151 TEX\\*.TM2 files the US release redrew (rd_hybrid.py). But many
characters (people in the maps, some of the cast) keep their pictures inside bigger files - AREAxx\\*.DAT, MOTCMN\\*.DAT and the
like - which also hold the maps and models made for each release's own program, so they cannot be swapped whole.

This compares every file both discs have under the same name, picture by picture (the TIM2 pictures in order); where a picture
has the same size on both but other pixels or other colours, the Japanese one is written as a texture override of the US one
(the engine knows a picture by the key of its pixels, as gpu.cpp's content_key computes it):

  FOLDER\\jp\\<key>.png          the Japanese picture, at the size the game draws it (a power of two each way)
  FOLDER\\jp_index.tsv           its lines for textures_index.tsv (the launcher adds them while the Japanese faces are on)

Pictures whose size changed between the releases are left as they are (the program of each release draws them its own way).
"""
import argparse, os, struct, sys, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rd_hybrid import Iso, SECTOR, FAT_HDR  # noqa: E402
from rd_prompts import content_key, png  # noqa: E402


def root(iso):
    """{name: (offset in ROOT.DAT, size)}, and ROOT.DAT's first sector"""
    fe, fs, _ = iso.find("/ROOT.FAT")
    de, ds, _ = iso.find("/ROOT.DAT")
    fat = iso.bytes(fe, fs)
    n = struct.unpack_from("<I", fat, 4)[0]
    first, names_at = struct.unpack_from("<II", fat, FAT_HDR - 12)
    out = {}
    for i in range(n):
        off, size, no = struct.unpack_from("<III", fat, first + 12 * i)
        at = no if no >= names_at else names_at + no
        out[fat[at:fat.index(b"\0", at)].decode("latin-1").upper()] = (off, size)
    return out, de


def tims(d):
    """(position, header fields, pixels, palette) of each TIM2 picture in a file, in order"""
    out, i = [], 0
    while True:
        i = d.find(b"TIM2", i)
        if i < 0 or i + 64 > len(d):
            return out
        tot, clsz, imsz, hdr, clc, fmt, mip, cltype, imtype, w, h = struct.unpack_from("<IIIHHBBBBHH", d, i + 16)
        if 0 < w <= 1024 and 0 < h <= 1024 and 0 < tot <= len(d) - i - 16 and hdr >= 48:
            pix = d[i + 16 + hdr:i + 16 + hdr + imsz]
            pal = d[i + 16 + hdr + imsz:i + 16 + hdr + imsz + clsz]
            out.append((i, (w, h, imtype, cltype, clc), pix, pal))
            i += 16 + tot
        else:
            i += 4


def colour(v, bits):
    if bits == 16:
        r, g, b, a = (v & 31) << 3, ((v >> 5) & 31) << 3, ((v >> 10) & 31) << 3, 255 if v & 0x8000 else 0
        return r, g, b, a
    if bits == 24:
        return v & 255, (v >> 8) & 255, (v >> 16) & 255, 255
    return v & 255, (v >> 8) & 255, (v >> 16) & 255, min(255, ((v >> 24) & 255) * 2)


def decode(head, pix, pal):
    """RGBA rows of a TIM2 picture (4/8-bit with a palette, 16/24/32-bit direct), or None for kinds not handled"""
    w, h, imtype, cltype, clc = head
    cbits = {1: 16, 2: 24, 3: 32}.get(cltype & 0x3F, 32)
    palette = []
    if imtype in (4, 5):
        step = cbits // 8
        palette = [colour(int.from_bytes(pal[k * step:k * step + step], "little"), cbits) for k in range(len(pal) // step)]
        if imtype == 5 and len(palette) == 256 and not (cltype & 0x80):
            # the GS stores 256-colour palettes in blocks of 8: entries 8-15 and 16-23 of each 32 are swapped
            fixed = list(palette)
            for k in range(256):
                b = k & 0x18
                j = (k & ~0x18) | (0x10 if b == 0x08 else 0x08 if b == 0x10 else b)
                fixed[k] = palette[j]
            palette = fixed
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            k = y * w + x
            if imtype == 4:
                b = pix[k >> 1] if (k >> 1) < len(pix) else 0
                c = palette[(b >> 4) if k & 1 else (b & 15)] if palette else (0, 0, 0, 0)
            elif imtype == 5:
                c = palette[pix[k]] if k < len(pix) and pix[k] < len(palette) else (0, 0, 0, 0)
            elif imtype == 1:
                c = colour(int.from_bytes(pix[k * 2:k * 2 + 2], "little"), 16)
            elif imtype == 2:
                c = colour(int.from_bytes(pix[k * 3:k * 3 + 3], "little"), 24)
            elif imtype == 3:
                c = colour(int.from_bytes(pix[k * 4:k * 4 + 4], "little"), 32)
            else:
                return None
            row.append(c)
        rows.append(row)
    return rows


def pow2(n):
    p = 1
    while p < n:
        p *= 2
    return p


def write_png(path, rows, w, h):
    tw, th = pow2(w), pow2(h)
    raw = bytearray()
    for y in range(th):
        raw += b"\0"
        if y < h:
            for (r, g, b, a) in rows[y]:
                raw += bytes((r, g, b, a))
            raw += bytes(4 * (tw - w))
        else:
            raw += bytes(4 * tw)
    png(path, tw, th, bytes(raw))
    return tw, th


def make(us_path, jp_path, out):
    us, jp = Iso(us_path), Iso(jp_path)
    ue, ude = root(us)
    je, jde = root(jp)
    os.makedirs(os.path.join(out, "jp"), exist_ok=True)
    lines, seen, files = [], set(), 0

    def add(a_pix, head_jp, pix_jp, pal_jp):
        key = content_key(a_pix)
        if key in seen:
            return False
        rows = decode(head_jp, pix_jp, pal_jp)
        if rows is None:
            return False
        seen.add(key)
        tw, th = write_png(os.path.join(out, "jp", "%016x.png" % key), rows, head_jp[0], head_jp[1])
        lines.append("%016x\t%d\t%d\t0\t0\tjp/%016x.png\n" % (key, tw, th, key))
        return True

    # the TEX\ pictures the US release redrew (jp_textures.txt), and its US-only copies paired with a Japanese one (third column):
    # as overrides too, so the look changes at once in the game (the disc's swap needs the game to load the picture again)
    listing = os.path.join(os.path.dirname(os.path.abspath(__file__)), "jp_textures.txt")
    tex = 0
    if os.path.exists(listing):
        for line in open(listing, encoding="utf-8"):
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split("\t")
            uname = parts[0].replace("/", "\\").upper()
            jname = (parts[2] if len(parts) > 2 else parts[0]).strip().replace("/", "\\").upper()
            if uname not in ue or jname not in je:
                continue
            us.f.seek(ude * SECTOR + ue[uname][0])
            a = us.f.read(ue[uname][1])
            jp.f.seek(jde * SECTOR + je[jname][0])
            b = jp.f.read(je[jname][1])
            ta, tb = tims(a), tims(b)
            if not ta or not tb or ta[0][1][:2] != tb[0][1][:2]:
                continue
            if (ta[0][2], ta[0][3]) != (tb[0][2], tb[0][3]) and add(ta[0][2], tb[0][1], tb[0][2], tb[0][3]):
                tex += 1
    for name in sorted(ue):
        if name.startswith("TEX\\") or name not in je:
            continue   # TEX\ is swapped whole by rd_hybrid.py
        uoff, usize = ue[name]
        joff, jsize = je[name]
        if usize > 16 << 20 or jsize > 16 << 20:
            continue
        us.f.seek(ude * SECTOR + uoff)
        a = us.f.read(usize)
        jp.f.seek(jde * SECTOR + joff)
        b = jp.f.read(jsize)
        if a == b or b"TIM2" not in a:
            continue
        ta, tb = tims(a), tims(b)
        if len(ta) != len(tb):
            continue
        changed = 0
        for (_, ha, pa, ca), (_, hb, pb, cb) in zip(ta, tb):
            if ha[:3] != hb[:3] or (pa == pb and ca == cb):
                continue
            if add(pa, hb, pb, cb):
                changed += 1
        if changed:
            files += 1
    with open(os.path.join(out, "jp_index.tsv"), "w", newline="\n") as f:
        f.writelines(lines)
    print("%d pictures (%d from TEX\\, the rest from %d other files) written to %s" % (len(lines), tex, files, out))
    return 0


def selftest():
    head = (2, 1, 4, 3, 16)
    pal = struct.pack("<16I", *([0x80FFFFFF] + [0] * 15))
    rows = decode(head, bytes([0x10]), pal)
    assert rows[0][0] == (255, 255, 255, 255) and rows[0][1][3] == 0
    assert pow2(72) == 128 and pow2(64) == 64
    print("selftest ok")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make")
    m.add_argument("--us", required=True)
    m.add_argument("--jp", required=True)
    m.add_argument("--out", required=True)
    sub.add_parser("selftest")
    a = ap.parse_args()
    return make(a.us, a.jp, a.out) if a.cmd == "make" else selftest()


if __name__ == "__main__":
    sys.exit(main())
