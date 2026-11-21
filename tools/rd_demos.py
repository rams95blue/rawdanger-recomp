#!/usr/bin/env python3
"""
rd_demos.py - compare the two Japanese demo discs (Taikenban Type-A / Type-B) with the retail games.

    python rd.py demos            (finds the discs in GAME US / GAME JP / GAME JP/DEMO)
    python tools/rd_demos.py --us US.iso --jp JP.iso --demo-a A.cue --demo-b B.cue [--out report.md]

Reads .iso (2048-byte sectors) and .bin/.cue (2352-byte raw sectors, MODE1 or MODE2) without extracting anything.
For each disc it reads the boot ELF named in SYSTEM.CNF and ROOT.FAT, then reports:
  - which game scenes (the 24-byte scene table: name, init, update, ...) each build has,
  - which map names each program knows (AREAxx\\Mx_xx_xx) and which of those have data on that disc,
  - map data a demo disc carries that the retail discs do not (content cut or changed before release),
  - debug / developer strings only a demo program has.
"""
from __future__ import annotations

import argparse
import os
import re
import struct
import sys

FAT_HDR = 0x100


class Disc:
    def __init__(self, path):
        if path.lower().endswith(".cue"):
            path = self._bin_from_cue(path)
        self.path = path
        self.f = open(path, "rb")
        self.raw, self.data_off = 2048, 0
        for raw, off in ((2048, 0), (2352, 24), (2352, 16)):
            self.raw, self.data_off = raw, off
            pvd = self.read(16, 1)
            if pvd[1:6] == b"CD001" and pvd[0] == 1:
                break
        else:
            sys.exit(f"{path}: not an ISO 9660 disc image (.iso or .bin)")
        self.root = pvd[156:156 + 34]

    @staticmethod
    def _bin_from_cue(cue):
        text = open(cue, encoding="utf-8", errors="replace").read()
        m = re.search(r'FILE\s+"([^"]+)"', text) or re.search(r"FILE\s+(\S+)", text)
        if not m:
            sys.exit(f"{cue}: no FILE line")
        return os.path.join(os.path.dirname(os.path.abspath(cue)), m.group(1))

    def read(self, lba, count):
        if self.raw == 2048:
            self.f.seek(lba * 2048)
            return self.f.read(count * 2048)
        out = bytearray()
        for i in range(count):
            self.f.seek((lba + i) * self.raw + self.data_off)
            out += self.f.read(2048)
        return bytes(out)

    def bytes(self, extent, size):
        return self.read(extent, (size + 2047) // 2048)[:size]

    def records(self, lba, size):
        data = self.read(lba, (size + 2047) // 2048)
        pos = 0
        while pos < len(data):
            n = data[pos]
            if n == 0:
                pos = (pos // 2048 + 1) * 2048
                continue
            rec = data[pos:pos + n]
            name = rec[33:33 + rec[32]].decode("ascii", "replace").split(";")[0]
            yield name, struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
            pos += n

    def file(self, path):
        extent, size = struct.unpack_from("<I", self.root, 2)[0], struct.unpack_from("<I", self.root, 10)[0]
        for part in [p for p in path.replace("\\", "/").strip("/").split("/") if p]:
            for name, ext, length in self.records(extent, size):
                if name.upper() == part.upper():
                    extent, size = ext, length
                    break
            else:
                return None
        return self.bytes(extent, size)

    def boot_elf(self):
        cnf = (self.file("SYSTEM.CNF") or b"").decode("ascii", "replace")
        m = re.search(r"cdrom0?:\\?([^;\s]+)", cnf, re.I)
        name = m.group(1).strip("\\") if m else None
        return name, (self.file(name) if name else None)

    def fat_names(self):
        data = self.file("ROOT.FAT")
        if not data or data[:4] != b"FAT ":
            return set()
        n = struct.unpack_from("<I", data, 4)[0]
        out = set()
        for i in range(n):
            no = struct.unpack_from("<I", data, FAT_HDR + i * 12 + 8)[0]
            out.add(data[no:data.index(b"\0", no)].decode("ascii", "replace").upper())
        return out


def scenes(elf):
    """Scene names from the scene table: runs of fixed-size entries that start with a pointer to an upper-case name."""
    if not elf or elf[:4] != b"\x7fELF":
        return []
    phoff, phnum = struct.unpack_from("<I", elf, 28)[0], struct.unpack_from("<H", elf, 44)[0]
    segs = []
    for i in range(phnum):
        t, off, vaddr, _, filesz = struct.unpack_from("<IIIII", elf, phoff + i * 32)
        if t == 1:
            segs.append((vaddr, off, filesz))

    def at(addr):
        for v, o, s in segs:
            if v <= addr < v + s:
                return o + addr - v
        return None

    def cstr(addr):
        o = at(addr)
        if o is None:
            return None
        end = elf.find(b"\0", o, o + 24)
        s = elf[o:end] if end > o else b""
        return s.decode("ascii").strip() if re.fullmatch(rb"[A-Z][A-Z0-9_ ]{2,15}", s) else None

    best = []
    for v, o, s in segs:
        for start in range(o, o + s - 24 * 8, 4):
            if cstr(struct.unpack_from("<I", elf, start)[0]) != "DUMMY":
                continue
            for stride in (24, 20, 28, 16):  # retail and Type-B use 24-byte entries, Type-A 20
                names, p = [], start
                while p + stride <= o + s:
                    name = cstr(struct.unpack_from("<I", elf, p)[0])
                    if not name:
                        break
                    names.append(name)
                    p += stride
                if len(names) > len(best):
                    best = names
    return best


def maps(elf):
    return sorted(set(m.decode().upper() for m in re.findall(rb"AREA0\d\\M\d_\d\d_\d\d", elf or b"")))


def strings(elf):
    return set(m.group().decode("latin1") for m in re.finditer(rb"[\x20-\x7e]{6,}", elf or b""))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--us"); ap.add_argument("--jp"); ap.add_argument("--demo-a"); ap.add_argument("--demo-b")
    ap.add_argument("--out", help="also write the report to this Markdown file")
    a = ap.parse_args(argv)
    discs = {k: v for k, v in (("US", a.us), ("JP", a.jp), ("Type-A", a.demo_a), ("Type-B", a.demo_b)) if v}
    if len(discs) < 2:
        sys.exit("give at least two discs (--us/--jp/--demo-a/--demo-b)")

    info = {}
    for k, path in discs.items():
        d = Disc(path)
        name, elf = d.boot_elf()
        info[k] = dict(elf_name=name, elf=elf, scenes=scenes(elf), maps=maps(elf), fat=d.fat_names(),
                       strings=strings(elf))
        print(f"read {k}: {name}, {len(elf or b'')} bytes, {len(info[k]['fat'])} ROOT.FAT entries", file=sys.stderr)

    retail = [k for k in ("JP", "US") if k in info]
    demos = [k for k in ("Type-A", "Type-B") if k in info]
    ref = retail[0] if retail else list(info)[0]
    out = ["# Raw Danger demos vs retail", ""]
    out.append("| disc | program | size | scenes | maps known | ROOT.FAT entries |")
    out.append("|---|---|---|---|---|---|")
    for k, v in info.items():
        out.append(f"| {k} | {v['elf_name']} | {len(v['elf'] or b'')} | {len(v['scenes'])} | {len(v['maps'])} | {len(v['fat'])} |")
    out.append("")

    all_scenes = info[ref]["scenes"]
    for k in info:
        if k == ref:
            continue
        missing = [s for s in all_scenes if s not in info[k]["scenes"]]
        extra = [s for s in info[k]["scenes"] if s not in all_scenes]
        out.append(f"## {k} scenes vs {ref}")
        out.append(f"- missing: {', '.join(missing) or 'none'}")
        out.append(f"- only in {k}: {', '.join(extra) or 'none'}")
        out.append("")

    retail_fat = set().union(*(info[k]["fat"] for k in retail)) if retail else set()
    retail_maps = set().union(*(info[k]["maps"] for k in retail)) if retail else set()
    for k in demos:
        v = info[k]
        extra_maps = [m for m in v["maps"] if m not in retail_maps]
        out.append(f"## {k} maps")
        out.append(f"- program knows {len(v['maps'])} maps: {' '.join(m.split(chr(92))[1] for m in v['maps'])}")
        for m in extra_maps:
            have = sorted(n for n in v["fat"] if n.startswith(m + ".") or n.startswith(m + "_"))
            out.append(f"- **{m}** is not in the retail game; on this disc: "
                       + (f"{len(have)} files ({', '.join(have[:6])}{' ...' if len(have) > 6 else ''})" if have else "no data (name only)"))
        if retail_fat:
            new = sorted(n for n in v["fat"] - retail_fat)
            out.append(f"- {len(new)} ROOT.FAT entries are not on the retail discs"
                       + (": " + ", ".join(new[:40]) + (" ..." if len(new) > 40 else "") if new else ""))
        dev = sorted(s for s in v["strings"] - set().union(*(info[r]["strings"] for r in retail))
                     if re.search(r"%|Error|DEBUG|TEST|byte", s) and len(s) < 80)
        if dev:
            out.append(f"- debug/developer strings only in {k}: " + " | ".join(dev[:25]))
        out.append("")

    report = "\n".join(out)
    print(report)
    if a.out:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        open(a.out, "w", encoding="utf-8").write(report)
        print(f"wrote {a.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
