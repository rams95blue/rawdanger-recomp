#!/usr/bin/env python3
"""
rd_eu.py - what the Enhanced Edition takes from the European disc (Raw Danger!, SLES-54587, 505 Games 2007).

The European release is the US game a year later, with a few things put right (docs/VERSION_DIFFERENCES.md). The Enhanced
Edition takes those from your EU disc image when it makes its disc (tools/rd_hybrid.py --eu):

  - the "Survived" stamp in English: the character select marks each hero you got through the game with it (CHARSEL.DAT,
    picture 40; the US disc has the Japanese 生還 there). Not taken: the "TP" -> "BT" label (BOOT.DAT, IMENU, CHARSEL);
  - the documents' texts (in the program): the Capitol Newspaper of December 24 (in the US program that document's text
    pointer points to Geo's guidelines, so the newspaper shows Geo's guidelines a second time), "collect several key items"
    in the investigation report, and two "[[" escapes ([Investigation Report], [Review]) the US texts lack;
  - the typos the EU release corrected in the dialogue (the code overlays): "chief", "your mother's", "through", "seen him",
    "What are you going", the game result screen's "Would you like to save the data?", and two lines made to fit their box.

Not taken: the EU tutorial line and its voice (VOICE\\MES_01_99_00_073_EVE.VOI): the European disc has a line from another
part of the tutorial there ("If you lose your footing ...") in place of "Tilt the left analog stick to Walk".

The program's texts go in as a patch file (written into the program before it is translated, and again every frame), the rest is
written into the new disc's ROOT.DAT. Nothing from the EU disc is in this project: it is read from your image each time.

  rd_eu.py --us "Raw Danger (USA).iso" --eu "Raw Danger (Europe).iso" [--pnach eu.pnach]
      says what it would change (and writes the patch file with --pnach); rd_hybrid.py --eu does the same while it makes the
      disc.
"""
from __future__ import annotations

import argparse
import os
import struct
import sys

SECTOR = 2048
FAT_HDR = 0x100
SECTION = "EU text fixes"

# The dialogue lines the EU release corrected: (overlay, the US words, the words used). The EU words are looked up on the EU
# disc before they are used (they are what it says, except where noted).
OVERLAY_TEXT = [
    ("OVERLAY\\DISC\\M1_01_01.BIN", "Are you worried about chef? ", "Are you worried about chief? ", None),
    ("OVERLAY\\DISC\\M1_03_00.BIN", "She gave away you mother's memento!", "She gave away your mother's memento!", None),
    ("OVERLAY\\DISC\\M1_06_00.BIN", "I can see you've gone though\na lot of trouble to find me.",
     "I can see you've gone through\na lot of trouble to find me.", None),
    ("OVERLAY\\DISC\\M2_04_00.BIN", "We've both been so busy. \nI haven't seen for a while.",
     "We've both been so busy. \nI haven't seen him for a while.", None),
    # the EU words ("Vegetable oil? \nWhat are you going to do with it?") are 2 bytes longer than the room the US line has
    ("OVERLAY\\DISC\\M4_02_00.BIN", "Vegetable oil? \nWhat are going to do with it?",
     "Vegetable oil? \nWhat are you going to do with it?", "Vegetable oil?\nWhat're you going to do with it?"),
    ("OVERLAY\\DISC\\GAMERESULT.BIN", "Do you wish to delete saved data?", "Would you like to save the data?", None),
    # too long for the text box in the US release: one line broken in two, one word left out
    ("OVERLAY\\DISC\\AREA04.BIN", "We have a new student teacher. Her name is Kelly Austin. She will teach English.",
     "We have a new student teacher. Her name\nis Kelly Austin. She will teach English.", None),
    ("OVERLAY\\DISC\\M3_11_00.BIN", "I want to get out of this dangerous place right now.",
     "I want to get out of this place right now.", None),
]

# The character select's stamp on a hero who survived (CHARSEL.DAT's TIM2 pictures, counted from 0): 生還 on the US disc.
STAMP_PICTURE = 40

# The documents' texts: the pointer column of the table (one entry every 0x20 bytes), in each program.
DOC_TABLE_US, DOC_TABLE_EU, DOC_COUNT = 0x3CC754, 0x3CC6D4, 33
# Room for texts that do not fit where the US text is: the top 8 KB of the memory below the program (0x80000-0x100000 is user
# memory the game never touches: zero in RAM in all 18 save states checked, title, menus, maps, tutorial, endings). The pnach
# tools make it a segment of the program, loaded with it. (The zero runs inside the program are not free: tables and pictures.)
FREE_US = (0x000FE000, 0x00100000)


class Elf:
    def __init__(self, data):
        self.data = data
        phoff, = struct.unpack_from("<I", data, 28)
        n, = struct.unpack_from("<H", data, 44)
        self.segs = []
        for i in range(n):
            kind, off, va, _, filesz, _ = struct.unpack_from("<6I", data, phoff + i * 32)
            if kind == 1 and filesz:
                self.segs.append((va, off, filesz))

    def at(self, va, n):
        for v, off, size in self.segs:
            if v <= va and va + n <= v + size:
                return off + va - v
        return None

    def read(self, va, n):
        at = self.at(va, n)
        return None if at is None else self.data[at:at + n]

    def word(self, va):
        b = self.read(va, 4)
        return None if b is None else struct.unpack("<I", b)[0]

    def text(self, va):
        at = self.at(va, 1)
        if at is None:
            return None
        end = self.data.find(b"\0", at)
        return self.data[at:end]


def elf_fixes(us_data, eu_data):
    """{address: bytes} for the US program (texts and pointers), and what they are, from the EU program."""
    us, eu = Elf(us_data), Elf(eu_data)
    writes, notes = {}, []
    first_us, first_eu = us.word(DOC_TABLE_US), eu.word(DOC_TABLE_EU)
    if (first_us is None or first_eu is None or not (us.text(first_us) or b"").startswith(b"Employee list")
            or not (eu.text(first_eu) or b"").startswith(b"Employee list")):
        return {}, ["the programs are not the ones expected (SLUS-21501, SLES-54587): the program's texts are left as they are"]
    free = FREE_US[0]
    ptrs = [us.word(DOC_TABLE_US + 0x20 * i) for i in range(DOC_COUNT)]
    for i in range(DOC_COUNT):
        pu, pe = ptrs[i], eu.word(DOC_TABLE_EU + 0x20 * i)
        if pu is None or pe is None:
            return {}, ["the document tables are not where expected: the program's texts are left as they are"]
        a, b = us.text(pu), eu.text(pe)
        if a is None or b is None or a == b:
            continue
        # room in place: the US text, its zero and the zeros after it (the padding to the next text)
        at = us.at(pu, 1)
        room = len(a)
        while us.data[at + room] == 0 and room < len(a) + 64:
            room += 1
        shared = ptrs.count(pu) > 1
        title = b.split(b"\n", 1)[0].strip().decode("latin-1")
        title = title[2:-1] if title.startswith("[[") else title[1:-1] if title.startswith("<") else title
        if not shared and len(b) + 1 <= room:
            writes[pu] = b + bytes(len(a) + 1 - len(b)) if len(b) < len(a) else b + b"\0"
            notes.append(f"document {i} ({title}): the EU text, in place")
        else:
            size = (len(b) + 1 + 15) & ~15
            if free + size > FREE_US[1]:
                notes.append(f"document {i} ({title}): no room, left as it is")
                continue
            writes[free] = b + bytes(size - len(b))
            writes[DOC_TABLE_US + 0x20 * i] = struct.pack("<I", free)
            what = "its own text (the US entry points to another document's)" if shared else "the EU text"
            notes.append(f"document {i} ({title}): {what}, at {free:08X}")
            free += size
    return writes, notes


def pnach_text(writes, us_data):
    """The writes as pnach word lines. patch=1 (every frame, besides being written into the program before it is translated): a
    save state made before these texts were in keeps the old ones in its memory, and gets them back this way."""
    us = Elf(us_data)
    words = {}
    for addr, data in writes.items():
        for k, byte in enumerate(data):
            words.setdefault((addr + k) & ~3, {})[(addr + k) & 3] = byte
    lines = []
    for w in sorted(words):
        was = us.read(w, 4) or bytes(4)
        new = bytearray(was)
        for k, byte in words[w].items():
            new[k] = byte
        if bytes(new) != was:
            lines.append(f"patch=1,EE,{w:08X},word,{struct.unpack('<I', bytes(new))[0]:08X}")
    return lines


def write_pnach(path, writes, notes, us_data):
    lines = pnach_text(writes, us_data)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("gametitle=Raw Danger! (SLUS-21501) - texts from the European release, made by tools/rd_eu.py from your EU disc\n\n")
        f.write(f"[{SECTION}]\nauthor=rawdanger-recomp\ndescription=The documents' texts as the European release has them\n")
        for n in notes:
            f.write(f"// {n}\n")
        f.write("\n".join(lines) + ("\n" if lines else ""))
    return len(lines)


def root_index(iso):
    """ROOT.FAT: {NAME: (offset in ROOT.DAT, size)} and ROOT.DAT's (extent, size)."""
    fe, fs, _ = iso.find("/ROOT.FAT")
    de, ds, _ = iso.find("/ROOT.DAT")
    fat = iso.bytes(fe, fs)
    if fat[:4] != b"FAT ":
        sys.exit(f"{iso.path}: ROOT.FAT is not a FAT index")
    n = struct.unpack_from("<I", fat, 4)[0]
    first, names_at = struct.unpack_from("<II", fat, FAT_HDR - 12)
    out = {}
    for i in range(n):
        off, size, no = struct.unpack_from("<III", fat, first + 12 * i)
        at = no if no >= names_at else names_at + no
        out[fat[at:fat.index(b"\0", at)].decode("latin-1").upper()] = (off, size)
    return out, (de, ds)


def root_file(iso, index, name):
    off, size = index[0][name]
    iso.f.seek(index[1][0] * SECTOR + off)
    return iso.f.read(size)


def text_fix(usd, eud, us_words, eu_words, used):
    """usd with us_words replaced by `used` (or None and why)."""
    u, e, w = us_words.encode("latin-1"), eu_words.encode("latin-1"), (used or eu_words).encode("latin-1")
    at = usd.find(u + b"\0")
    if at < 0 or usd.find(u + b"\0", at + 1) >= 0:
        return None, "not found once in the US file"
    if eud.find(e + b"\0") < 0:
        return None, "not on the EU disc"
    room = len(u)
    while at + room < len(usd) and usd[at + room] == 0 and room < len(u) + 64:
        room += 1
    if len(w) + 1 > room:
        return None, f"{len(w)} letters, room for {room - 1}"
    out = bytearray(usd)
    out[at:at + max(len(u), len(w)) + 1] = w + bytes(max(len(u), len(w)) + 1 - len(w))
    return bytes(out), None


def tims(d):
    out, p = [], 0
    while True:
        p = d.find(b"TIM2", p)
        if p < 0:
            return out
        out.append(p)
        p += 4


def picture_colours(d, at):
    """A TIM2 picture's pixels as (r, g, b, a) (a: 0-128, the GS's scale), for 4- and 8-bit pictures with a 32-bit palette."""
    tot, cs, isz, hs, cc, fmt, mm, ct, it, w, h = struct.unpack_from("<IIIHHBBBBHH", d, at + 16)
    q = at + 16
    img, clut = d[q + hs:q + hs + isz], d[q + hs + isz:q + hs + isz + cs]
    if ct & 0x7F != 3 or it not in (4, 5):
        return None
    if it == 5:   # 8 bits: the palette in the GS's order (entries 8-15 and 16-23 of each 32 swapped) unless bit 0x80 says linear
        idx = [v if ct & 0x80 else (v & ~0x18) | ((v & 0x08) << 1) | ((v & 0x10) >> 1) for v in img]
    else:
        idx = [(img[i // 2] >> 4) if i & 1 else (img[i // 2] & 15) for i in range(w * h)]
    return w, h, [tuple(clut[4 * s:4 * s + 4]) for s in idx]


def sixteen(pixels, k=16, rounds=30):
    """The k colours that stand for the picture's colours best (weighted k-means: the EU stamp has 129 shades of one red)."""
    import collections
    count = collections.Counter(pixels)
    cols = list(count)
    if len(cols) <= k:
        return cols + [(0, 0, 0, 0)] * (k - len(cols))
    def d2(a, b): return sum((x - y) ** 2 for x, y in zip(a, b))
    cent = [max(cols, key=lambda c: count[c])]
    while len(cent) < k:
        cent.append(max(cols, key=lambda c: count[c] ** 0.5 * min(d2(c, m) for m in cent)))
    for _ in range(rounds):
        acc = [[0, 0, 0, 0, 0] for _ in cent]
        for c in cols:
            j = min(range(k), key=lambda j: d2(c, cent[j]))
            for t in range(4):
                acc[j][t] += c[t] * count[c]
            acc[j][4] += count[c]
        new = [tuple(round(a[t] / a[4]) for t in range(4)) if a[4] else cent[j] for j, a in enumerate(acc)]
        if new == cent:
            break
        cent = new
    return cent


def picture_as_us(usd, eud, k):
    """usd with its picture k drawn as the EU file's picture k, in the US picture's own format and place (the US file keeps its
    size and layout). The EU "Survived" stamp is 8-bit (256 colours), the US one 4-bit (16): it is brought down to 16."""
    tu, te = tims(usd), tims(eud)
    if k >= len(tu) or k >= len(te):
        return usd
    a, b = tu[k], te[k]
    tot, cs, isz, hs, cc, fmt, mm, ct, it, w, h = struct.unpack_from("<IIIHHBBBBHH", usd, a + 16)
    src = picture_colours(eud, b)
    if src is None or src[:2] != (w, h) or it != 4 or cc != 16 or cs != 64 or isz != (w * h + 1) // 2:
        return usd
    pal = sixteen(src[2])
    def d2(p, q): return sum((x - y) ** 2 for x, y in zip(p, q))
    near = {c: min(range(16), key=lambda j: d2(c, pal[j])) for c in set(src[2])}
    idx = [near[c] for c in src[2]] + [0]
    img = bytes(idx[i] | idx[i + 1] << 4 for i in range(0, w * h, 2))
    out = bytearray(usd)
    q = a + 16 + hs
    out[q:q + isz] = img
    out[q + isz:q + isz + cs] = b"".join(bytes(c) for c in pal)
    return bytes(out)


def root_fixes(us_iso, eu_iso, with_tims=None):
    """{NAME: new file} for the US ROOT.DAT (same sizes), and what they are."""
    ui, ei = root_index(us_iso), root_index(eu_iso)
    files, notes = {}, []
    stamp = "CHARSEL\\CHARSEL.DAT"
    if stamp in ui[0] and stamp in ei[0]:
        usd, eud = root_file(us_iso, ui, stamp), root_file(eu_iso, ei, stamp)
        new = picture_as_us(usd, eud, STAMP_PICTURE)
        if new != usd:
            files[stamp] = new
            notes.append(f"CHARSEL.DAT: the English \"Survived\" stamp (picture {STAMP_PICTURE})")
        else:
            notes.append("CHARSEL.DAT: the stamp is not where expected, left as it is")
    for name, us_words, eu_words, used in OVERLAY_TEXT:
        if name not in ui[0] or name not in ei[0]:
            notes.append(f"{name}: not on both discs")
            continue
        usd = files.get(name) or root_file(us_iso, ui, name)
        new, why = text_fix(usd, root_file(eu_iso, ei, name), us_words, eu_words, used)
        short = (used or eu_words).replace("\n", " ")
        if new is None:
            notes.append(f"{name.split(chr(92))[-1]}: \"{short}\" left out ({why})")
        else:
            files[name] = new
            notes.append(f"{name.split(chr(92))[-1]}: \"{short}\"")
    return files, notes


def program(iso):
    """The boot program's bytes (SYSTEM.CNF's BOOT2 line)."""
    ext, size, _ = iso.find("/SYSTEM.CNF")
    cnf = iso.bytes(ext, size).decode("latin-1")
    for line in cnf.splitlines():
        if line.strip().upper().startswith("BOOT2"):
            name = line.split("\\")[-1].split(";")[0].strip()
            pe, ps, _ = iso.find("/" + name)
            return name, iso.bytes(pe, ps)
    sys.exit(f"{iso.path}: no BOOT2 line in SYSTEM.CNF")


def check_eu(iso):
    name, _ = program(iso)
    if name.upper() != "SLES_545.87":
        sys.exit(f"{iso.path}: the boot program is {name}, not SLES_545.87 (Raw Danger!, Europe)")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--us", required=True)
    ap.add_argument("--eu", required=True)
    ap.add_argument("--pnach", default="")
    a = ap.parse_args(argv)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from rd_hybrid import Iso, with_tims
    us, eu = Iso(a.us), Iso(a.eu)
    check_eu(eu)
    files, notes = root_fixes(us, eu, with_tims)
    us_name, us_elf = program(us)
    _, eu_elf = program(eu)
    writes, enotes = elf_fixes(us_elf, eu_elf)
    for n in notes + enotes:
        print("  " + n)
    if a.pnach:
        print(f"{write_pnach(a.pnach, writes, enotes, us_elf)} words in {a.pnach}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
