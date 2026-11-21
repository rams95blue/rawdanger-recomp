#!/usr/bin/env python3
"""
rd_elf.py - inspect the Raw Danger boot ELF before recompiling it.

  info   <elf> [--manifest _rd_manifest.json] [-o elf_report.md]
         header, segments, sections, CRC32, SDK library versions, IRX modules
         loaded, disc files referenced by name, and an LBA-table heuristic
         (does the ELF hard-code sector numbers of disc files?)
  verify <elf> [--db patches/patchdb.json]
         check every patch site in patchdb.json for this ELF: code or data,
         current value, expected original value

stdlib only.
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import re
import struct
import sys
import zlib

SHF_EXECINSTR = 0x4
PF_X = 0x1


class Elf:
    def __init__(self, path: str):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
            raise ValueError("Not a 32-bit little-endian ELF")
        (self.type, self.machine, _, self.entry, self.phoff, self.shoff, self.flags, _,
         self.phentsize, self.phnum, self.shentsize, self.shnum, self.shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", d, 16)
        self.crc32 = zlib.crc32(d) & 0xFFFFFFFF
        padded = d + b"\0" * ((-len(d)) % 4)
        x = 0
        for (w,) in struct.iter_unpack("<I", padded):
            x ^= w
        self.pcsx2_crc = x  # PCSX2's game CRC: XOR of all 32-bit words
        self.segments = []
        for i in range(self.phnum):
            t, off, vaddr, paddr, filesz, memsz, flg, align = struct.unpack_from("<8I", d, self.phoff + i * self.phentsize)
            self.segments.append({"type": t, "offset": off, "vaddr": vaddr, "filesz": filesz, "memsz": memsz, "flags": flg})
        self.sections = []
        if self.shoff and self.shnum:
            raw = []
            for i in range(self.shnum):
                raw.append(struct.unpack_from("<10I", d, self.shoff + i * self.shentsize))
            strtab = raw[self.shstrndx] if self.shstrndx < len(raw) else None
            for r in raw:
                name = ""
                if strtab:
                    o = strtab[4] + r[0]
                    name = d[o:d.index(b"\0", o)].decode("ascii", "replace")
                self.sections.append({"name": name, "type": r[1], "flags": r[2], "addr": r[3], "offset": r[4], "size": r[5]})

    def vaddr_to_off(self, va: int) -> int | None:
        va &= 0x1FFFFFFF if va >= 0x80000000 else 0xFFFFFFFF
        for s in self.segments:
            if s["type"] == 1 and s["vaddr"] <= va < s["vaddr"] + s["filesz"]:
                return s["offset"] + (va - s["vaddr"])
        return None

    def in_bss(self, va: int) -> bool:
        for s in self.segments:
            if s["type"] == 1 and s["vaddr"] + s["filesz"] <= va < s["vaddr"] + s["memsz"]:
                return True
        return False

    def read32(self, va: int) -> int | None:
        off = self.vaddr_to_off(va)
        if off is None or off + 4 > len(self.data):
            return None
        return struct.unpack_from("<I", self.data, off)[0]

    def classify(self, va: int) -> str:
        """'code', 'data', 'bss', 'outside', or 'unknown' (no section table and RWX segment)."""
        if self.sections:
            for s in self.sections:
                if s["addr"] and s["addr"] <= va < s["addr"] + s["size"]:
                    if s["flags"] & SHF_EXECINSTR and s["flags"] & 0x1:
                        return "unknown"  # one writable+executable blob (CodeWarrior style): no answer here
                    return "code" if s["flags"] & SHF_EXECINSTR else ("bss" if s["type"] == 8 else "data")
        if self.in_bss(va):
            return "bss"
        for s in self.segments:
            if s["type"] == 1 and s["vaddr"] <= va < s["vaddr"] + s["filesz"]:
                if s["flags"] & PF_X and not (s["flags"] & 0x2):
                    return "code"
                return "unknown"
        return "outside"

    def text_range(self):
        for s in self.sections:
            if s["name"] == ".text":
                return s["addr"], s["addr"] + s["size"]
        for s in self.segments:
            if s["type"] == 1 and s["flags"] & PF_X:
                return s["vaddr"], s["vaddr"] + s["filesz"]
        return None


def is_jump(w):
    """j or jr (incl. jr ra): control never falls through past the delay slot."""
    return w is not None and ((w >> 26) == 2 or ((w >> 26) == 0 and (w & 0x3F) == 8))


def has_delay_slot(w):
    """Any R5900 branch or jump - the next word is its delay slot."""
    if not w:
        return False
    op = w >> 26
    if op == 0:
        return (w & 0x3F) in (8, 9)                        # jr, jalr
    if op == 1:                                            # regimm: bltz/bgez(l)(al); not mtsab/mtsah
        return ((w >> 16) & 31) in (0, 1, 2, 3, 0x10, 0x11, 0x12, 0x13)
    if op in (2, 3, 4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17):   # j, jal, beq..bgtz, likely forms
        return True
    if op in (0x10, 0x11, 0x12) and ((w >> 21) & 31) == 8:  # bc0/bc1/bc2
        return True
    return False


def after_unconditional(read32, v):
    """True when v starts right after `j/jr` + its delay slot (+ nop padding).

    v itself must not be a delay slot: a function that starts there would be
    split from its jump, and the recompiler drops the delay-slot instruction
    (e.g. `jr ra; addiu sp, sp, 0xE0` leaves the caller's sp 0xE0 too low).
    """
    if has_delay_slot(read32(v - 4)):
        return False
    q = v - 4
    while q >= v - 16 and read32(q) == 0:
        q -= 4
    if q == v - 4:                     # no padding: q is the delay slot, the jump is before it
        return is_jump(read32(q - 4))
    return is_jump(read32(q)) or is_jump(read32(q - 4))


def in_delay_slot(read32, v):
    """v is the delay slot of the instruction before it."""
    return has_delay_slot(read32(v - 4))


def code_ranges(e: "Elf", window_words=1024):
    """Estimate where real MIPS code lives in the executable load segment.

    CodeWarrior-style ELFs put code and data in ONE writable+executable segment,
    so recompilers treat floats/tables as instructions. Code is dense in
    `jr $ra` (03E00008) and `addiu $sp,$sp,imm` (27BDxxxx); data almost never
    contains the exact jr-ra word. Windows (4 KB) with both are code; adjacent
    code windows merge; each block ends right after its last jr ra + delay slot.
    Returns [(start, end)] and the segment's (start, end).
    """
    seg = next((s for s in e.segments if s["type"] == 1 and s["flags"] & PF_X and s["filesz"]), None)
    if not seg:
        return [], None
    base = seg["vaddr"]
    d = e.data[seg["offset"]:seg["offset"] + seg["filesz"]]
    n = len(d) // 4
    words = struct.unpack_from(f"<{n}I", d)
    is_code = []
    for w0 in range(0, n, window_words):
        chunk = words[w0:w0 + window_words]
        jr = sum(1 for w in chunk if w == 0x03E00008)
        sp = sum(1 for w in chunk if (w >> 16) == 0x27BD)
        is_code.append(jr > 0 and sp > 0)
    blocks = []
    i = 0
    while i < len(is_code):
        if not is_code[i]:
            i += 1
            continue
        j = i
        while j + 1 < len(is_code) and (is_code[j + 1] or (j + 2 < len(is_code) and is_code[j + 2])):
            j += 1
        lo_w, hi_w = i * window_words, min(n, (j + 1) * window_words)
        jrs = [k for k in range(lo_w, hi_w) if words[k] == 0x03E00008]
        start_w = lo_w
        if blocks or i > 0:  # islands: start at the first stack-frame setup
            start_w = next((k for k in range(lo_w, jrs[0] + 1) if (words[k] >> 16) == 0x27BD), lo_w)
        blocks.append((base + start_w * 4, base + (jrs[-1] + 2) * 4))
        i = j + 1
    if blocks:
        blocks[0] = (base, blocks[0][1])
    return blocks, (base, base + seg["filesz"])


def strings(data: bytes, minlen=5):
    for m in re.finditer(rb"[\x20-\x7e]{%d,}" % minlen, data):
        yield m.start(), m.group().decode("ascii")


def serial_from_name(name: str) -> str:
    b = os.path.basename(name).upper()
    return b.replace("_", "-", 1).replace(".", "")


def cmd_info(a):
    e = Elf(a.elf)
    L = []
    w = L.append
    w(f"# ELF report: {os.path.basename(a.elf)}\n")
    w(f"- Serial: **{serial_from_name(a.elf)}**")
    w(f"- File CRC32 (ps2recomp game-override key): **{e.crc32:08X}**")
    w(f"- PCSX2 CRC (patch databases): **{e.pcsx2_crc:08X}**")
    w(f"- Size: {len(e.data):,} bytes, entry 0x{e.entry:08X}, machine {e.machine} (8 = MIPS), flags 0x{e.flags:08X}")
    w("\n## Segments\n\n| type | vaddr | filesz | memsz | flags |\n|---|---|---:|---:|---|")
    for s in e.segments:
        fl = "".join(c for c, b in (("R", 4), ("W", 2), ("X", 1)) if s["flags"] & b)
        w(f"| {s['type']} | 0x{s['vaddr']:08X} | {s['filesz']:,} | {s['memsz']:,} | {fl} |")
    if e.sections:
        w("\n## Sections\n\n| name | addr | size | exec |\n|---|---|---:|:-:|")
        for s in e.sections:
            if s["name"]:
                w(f"| {s['name']} | 0x{s['addr']:08X} | {s['size']:,} | {'✓' if s['flags'] & SHF_EXECINSTR else ''} |")
    else:
        w("\n_No section table: code/data split will need Ghidra._")
    tr = e.text_range()
    if tr:
        w(f"\nText range: 0x{tr[0]:08X}–0x{tr[1]:08X} ({(tr[1] - tr[0]) // 4:,} instruction words)")

    allstr = list(strings(e.data))
    libs = sorted({s for _, s in allstr if s.startswith("PsII")})
    w("\n## SDK library version strings\n")
    w("\n".join(f"- `{s}`" for s in libs) or "- none found")
    irx = sorted({s for _, s in allstr if ".IRX" in s.upper()})
    w("\n## IRX modules referenced (these run on the IOP via ps2xIOP)\n")
    w("\n".join(f"- `{s}`" for s in irx) or "- none found")
    dev = sorted({s for _, s in allstr if re.search(r"(cdrom0|host0|mc0|rom0):", s, re.I)})
    w("\n## Device paths\n")
    w("\n".join(f"- `{s}`" for s in dev[:200]) or "- none found")

    if a.manifest:
        with open(a.manifest, encoding="utf-8") as f:
            man = json.load(f)
        files = man["files"]
        upper_data = e.data.upper()
        w("\n## Disc files referenced by name in the ELF\n")
        named = []
        for fi in files:
            leaf = os.path.basename(fi["path"]).upper().encode()
            if len(leaf) >= 4 and leaf in upper_data:
                named.append(fi)
        w("\n".join(f"- `{fi['path']}` (LBA {fi['lba']}, {fi['size']:,} B)" for fi in named) or "- none")
        w("\n## LBA-table heuristic\n")
        w("Counts aligned 32-bit occurrences of each file's start sector (LBA) inside the ELF. "
          "Many hits on big data files = the game reads by raw sector -> mount the ISO (`[disc] image`) "
          "and keep the original disc layout.\n")
        w("| file | LBA | size | hits | hits with size/sectors next to it |\n|---|---:|---:|---:|---:|")
        rows = []
        words = collections.Counter()
        pos_by_word = collections.defaultdict(list)
        for off in range(0, len(e.data) - 3, 4):
            v = struct.unpack_from("<I", e.data, off)[0]
            if 300 <= v < 0x200000:
                words[v] += 1
                if len(pos_by_word[v]) < 64:
                    pos_by_word[v].append(off)
        for fi in files:
            lba = fi["lba"]
            if lba < 300:
                continue
            hits = words.get(lba, 0)
            if not hits:
                continue
            sectors = (fi["size"] + 2047) // 2048
            paired = 0
            for off in pos_by_word[lba]:
                around = [struct.unpack_from("<I", e.data, o)[0] for o in (off - 8, off - 4, off + 4, off + 8) if 0 <= o <= len(e.data) - 4]
                if fi["size"] in around or sectors in around:
                    paired += 1
            rows.append((paired, hits, fi))
        rows.sort(key=lambda r: (-r[0], -r[1]))
        for paired, hits, fi in rows[:80]:
            w(f"| `{fi['path']}` | {fi['lba']} | {fi['size']:,} | {hits} | {paired} |")
        if not rows:
            w("| (no hits — the game probably opens files by name; extracted-folder mode is enough) | | | | |")
        strong = sum(1 for r in rows if r[0] > 0)
        w(f"\n**{strong}** files have an LBA paired with their size/sector count -> "
          + ("strong evidence of a hard-coded disc TOC: mount the ISO." if strong >= 3 else "weak/no evidence of a hard-coded TOC."))
    out = "\n".join(L) + "\n"
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(out)
        print(f"wrote {a.out}")
    else:
        print(out)
    return 0


def cmd_coderanges(a):
    e = Elf(a.elf)
    blocks, seg = code_ranges(e)
    if not seg:
        print("no executable load segment")
        return 1
    code = sum(b - a_ for a_, b in blocks)
    print(f"executable segment 0x{seg[0]:08X}-0x{seg[1]:08X} ({(seg[1] - seg[0]) / 1e6:.2f} MB)")
    for lo, hi in blocks:
        print(f"  code 0x{lo:08X}-0x{hi:08X}  ({(hi - lo) // 4:,} instructions)")
    print(f"code {code / 1e6:.2f} MB, data {(seg[1] - seg[0] - code) / 1e6:.2f} MB")
    return 0


def load_db(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def game_for_elf(db, elf_path):
    serial = serial_from_name(elf_path)
    g = db["games"].get(serial)
    if not g:
        raise SystemExit(f"{serial} not in patch db (known: {', '.join(db['games'])})")
    return serial, g


def verify_sites(e: Elf, game: dict):
    """Yield (patch, site, kind, current, status)."""
    for p in game["patches"]:
        for s in p["sites"]:
            addr = int(s["addr"], 16)
            val = int(s["value"], 16)
            exp = int(s["expect"], 16) if s.get("expect") else None
            kind = e.classify(addr)
            cur = e.read32(addr)
            if cur is None:
                status = "NOT-IN-FILE" if kind != "bss" else "BSS"
            elif exp is None:
                status = "UNVERIFIED"
            elif cur == exp:
                status = "OK"
            elif cur == val:
                status = "ALREADY-PATCHED"
            else:
                status = "MISMATCH"
            yield p, s, addr, val, exp, kind, cur, status


def cmd_verify(a):
    e = Elf(a.elf)
    db = load_db(a.db)
    serial, g = game_for_elf(db, a.elf)
    print(f"{serial}  {g['title']}  CRC32 {e.crc32:08X}  PCSX2 CRC {e.pcsx2_crc:08X}")
    if g.get("pcsx2_crc") and g["pcsx2_crc"].upper() != f"{e.pcsx2_crc:08X}":
        print(f"  WARNING: patch db targets PCSX2 CRC {g['pcsx2_crc']} - different build, expect refusals")
    bad = 0
    for p, s, addr, val, exp, kind, cur, status in verify_sites(e, g):
        curs = f"{cur:08X}" if cur is not None else "--------"
        exps = f"{exp:08X}" if exp is not None else "  (none)"
        print(f"  [{status:>15}] {p['group']:<16} 0x{addr:08X} {kind:<8} now {curs} expect {exps} -> {val:08X}  {s.get('note', '')}")
        if status in ("MISMATCH", "NOT-IN-FILE"):
            bad += 1
    if bad:
        print(f"{bad} site(s) failed verification - those patches will be refused.")
    return 1 if bad else 0


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info"); p.add_argument("elf"); p.add_argument("--manifest"); p.add_argument("-o", "--out")
    p.set_defaults(fn=cmd_info)
    p = sub.add_parser("coderanges"); p.add_argument("elf"); p.set_defaults(fn=cmd_coderanges)
    p = sub.add_parser("verify"); p.add_argument("elf"); p.add_argument("--db", default=os.path.join(here, "..", "patches", "patchdb.json"))
    p.set_defaults(fn=cmd_verify)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
