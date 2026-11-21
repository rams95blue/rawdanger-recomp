#!/usr/bin/env python3
"""
rd_strpack.py - build the hybrid Japanese voice archive (STR.FAT + STR.DAT).

Raw Danger keeps every voice line in /STR/STR.DAT, indexed by /STR/STR.FAT:

  0x000  "FAT " , u32 entry count, zero padding to 0x100
  0x100  count x { u32 dat_offset, u32 size, u32 name_offset }   (little endian)
  ....   NUL-terminated names, e.g. "VOICE\\TITLE.VOI"
  DAT entries are 2048-byte aligned.

Measured on the real discs: US 4694 entries, JP 4824; 4687 shared names,
137 JP-only (NPCs speaking the protagonists' names, etc.), 7 US-only.

The hybrid keeps the US index ORDER (so lookups by index or by name both hit
the same slot as on the US disc), swaps in the JP recording wherever the name
exists in JP, keeps the US recording for the 7 English-only lines (or mutes
them with --mute-us-only), and appends the 137 JP-only lines at the end.

Output goes to <out>/STR/STR.FAT + STR.DAT and voice_map.txt gets file routes,
so rd_game.cpp serves them when [audio] voice_language = jp.

  rd_strpack.py --us work/us --jp work/jp --out work/us/voice_jp
"""
from __future__ import annotations

import argparse
import os
import struct
import sys

ALIGN = 2048
HDR = 0x100


def read_fat(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"FAT ":
        sys.exit(f"{path}: not a FAT index")
    n = struct.unpack_from("<I", d, 4)[0]
    entries = []
    for i in range(n):
        off, size, no = struct.unpack_from("<III", d, HDR + i * 12)
        name = d[no:d.index(b"\0", no)].decode("ascii")
        entries.append((name, off, size))
    return entries


def write_fat(path, entries):
    """entries: [(name, off, size)] in final order."""
    n = len(entries)
    names = bytearray()
    name_offs = []
    base = HDR + n * 12
    for name, _, _ in entries:
        name_offs.append(base + len(names))
        names += name.encode("ascii") + b"\0"
    out = bytearray(b"FAT " + struct.pack("<I", n))
    out = out.ljust(HDR, b"\0")
    # 0xF4: where the entries start, 0xF8: where the names start. The game needs both (zero: it finds no voice line at all)
    struct.pack_into("<II", out, HDR - 12, HDR, base)
    for (name, off, size), no in zip(entries, name_offs):
        out += struct.pack("<III", off, size, no)
    out += names
    with open(path, "wb") as f:
        f.write(out)


def copy_range(src, off, size, dst):
    src.seek(off)
    left = size
    while left:
        chunk = src.read(min(8 << 20, left))
        if not chunk:
            raise IOError("unexpected EOF in source DAT")
        dst.write(chunk)
        left -= len(chunk)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--us", required=True, help="extracted US root (work/us)")
    ap.add_argument("--jp", required=True, help="extracted JP root (work/jp)")
    ap.add_argument("--out", required=True, help="voice pack folder (work/us/voice_jp)")
    ap.add_argument("--mute-us-only", action="store_true", help="silence English-only lines instead of keeping them")
    a = ap.parse_args(argv)

    us_fat = read_fat(os.path.join(a.us, "STR", "STR.FAT"))
    jp_fat = read_fat(os.path.join(a.jp, "STR", "STR.FAT"))
    jp_by = {n.upper(): (n, o, s) for n, o, s in jp_fat}
    us_names = {n.upper() for n, _, _ in us_fat}

    out_dir = os.path.join(a.out, "STR")
    os.makedirs(out_dir, exist_ok=True)
    dat_path = os.path.join(out_dir, "STR.DAT")
    plan = []  # (name, source 'us'/'jp'/'mute', src_off, size)
    for name, off, size in us_fat:
        j = jp_by.get(name.upper())
        if j:
            plan.append((name, "jp", j[1], j[2]))
        elif a.mute_us_only:
            plan.append((name, "mute", 0, 0))
        else:
            plan.append((name, "us", off, size))
    jp_only = [(n, "jp", o, s) for n, o, s in jp_fat if n.upper() not in us_names]
    plan += jp_only

    # header sanity: compare the first bytes of a few shared lines
    with open(os.path.join(a.us, "STR", "STR.DAT"), "rb") as us_dat, \
            open(os.path.join(a.jp, "STR", "STR.DAT"), "rb") as jp_dat:
        shared = [(n, o, s) for n, o, s in us_fat if n.upper() in jp_by][:3]
        for n, o, s in shared:
            us_dat.seek(o)
            jo = jp_by[n.upper()]
            jp_dat.seek(jo[1])
            print(f"  {n}: US head {us_dat.read(16).hex()}  ({s:,} B)")
            print(f"  {' ' * len(n)}  JP head {jp_dat.read(16).hex()}  ({jo[2]:,} B)")

        final = []
        total = 0
        with open(dat_path, "wb") as dst:
            for i, (name, src, off, size) in enumerate(plan, 1):
                pos = dst.tell()
                if src == "jp":
                    copy_range(jp_dat, off, size, dst)
                elif src == "us":
                    copy_range(us_dat, off, size, dst)
                final.append((name, pos, size))
                pad = (-dst.tell()) % ALIGN
                if pad:
                    dst.write(b"\0" * pad)
                total += size
                if i % 200 == 0 or i == len(plan):
                    sys.stdout.write(f"\r  writing {i}/{len(plan)}  {dst.tell() / 1e6:,.0f} MB")
                    sys.stdout.flush()
        print()
    write_fat(os.path.join(out_dir, "STR.FAT"), final)

    counts = {k: sum(1 for p in plan if p[1] == k) for k in ("jp", "us", "mute")}
    print(f"hybrid STR: {len(plan)} entries = {counts['jp'] - len(jp_only)} JP replacing US, "
          f"{len(jp_only)} JP-only appended, {counts['us']} US-only kept, {counts['mute']} muted")

    # routes for rd_game.cpp (keep any other routes already in the map)
    map_path = os.path.join(a.out, "voice_map.txt")
    keep = []
    if os.path.exists(map_path):
        with open(map_path, encoding="utf-8") as f:
            keep = [l.rstrip("\n") for l in f if "STR/STR." not in l.upper().replace("\\", "/")]
    keep += ["file /STR/STR.FAT = STR/STR.FAT", "file /STR/STR.DAT = STR/STR.DAT"]
    with open(map_path, "w", encoding="utf-8") as f:
        f.write("\n".join(keep) + "\n")
    print(f"wrote {os.path.join(out_dir, 'STR.FAT')} + STR.DAT and {map_path}")
    print("Set  [audio] voice_language = jp  in rawdanger.ini to use it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
