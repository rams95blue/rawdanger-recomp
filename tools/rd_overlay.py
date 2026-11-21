#!/usr/bin/env python3
"""
rd_overlay.py - Raw Danger's MWo3 code overlays.

The game keeps 79 code overlays in ROOT.DAT as OVERLAY\\DISC\\*.BIN. They are
linked for a fixed address (no relocations): the loader FUN_002c99f0(name,
slot) reads the whole file to the slot address from the table at 0x48BE00
(0x718A00, 0x738600, 0x745D00, 0x83FE00, 0x860600), then FUN_00100290 zeroes
the BSS (header +0x14) and runs the static constructors (+0x18/+0x1C).

  rd_overlay.py extract --root work/us --out work/overlays
      copies every OVERLAY\\DISC\\*.BIN out of ROOT.DAT and writes index.json
      (name, ROOT.DAT offset, size, sha1, first 64 header bytes).

  rd_overlay.py recomp --overlays work/overlays --elf work/us/SLUS_215.01
                       --recompiler <ps2_recomp> --out recomp/out/overlays
      recompiles every overlay at its load address (see below) and writes
      the runner sources to <out>/runner: ov_<NAME>_*.cpp, ov_<NAME>_functions.h
      and rd_overlay_tables.cpp (what rd_game.cpp registers at load time).

MWo3 header (64 bytes, little endian):
  +0x00 "MWo3"   +0x04 id        +0x08 load address
  +0x0C code size (code starts at load+0x40)   +0x10 data size
  +0x14 bss size  +0x18/+0x1C ctor/dtor table   +0x20 file name
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rd_elf import after_unconditional, in_delay_slot  # noqa: E402

HDR = 0x100


def read_fat(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"FAT ":
        sys.exit(f"{path}: not a FAT index")
    n = struct.unpack_from("<I", d, 4)[0]
    out = []
    for i in range(n):
        off, size, no = struct.unpack_from("<III", d, HDR + i * 12)
        out.append((d[no:d.index(b"\0", no)].decode("ascii", "replace"), off, size))
    return out


def cmd_extract(a):
    fat = os.path.join(a.root, "ROOT.FAT")
    dat = os.path.join(a.root, "ROOT.DAT")
    for p in (fat, dat):
        if not os.path.exists(p):
            sys.exit(f"missing {p} - run: python rd.py extract")
    entries = [e for e in read_fat(fat) if e[0].upper().startswith("OVERLAY\\") and e[0].upper().endswith(".BIN")]
    os.makedirs(a.out, exist_ok=True)
    index = []
    with open(dat, "rb") as f:
        for name, off, size in entries:
            f.seek(off)
            data = f.read(size)
            if len(data) != size:
                sys.exit(f"{name}: short read at 0x{off:X}")
            leaf = name.split("\\")[-1]
            with open(os.path.join(a.out, leaf), "wb") as o:
                o.write(data)
            index.append({"name": name, "file": leaf, "offset": off, "size": size,
                          "sha1": hashlib.sha1(data).hexdigest(), "head": data[:64].hex()})
    with open(os.path.join(a.out, "index.json"), "w", encoding="utf-8") as f:
        json.dump(index, f, indent=1)
    total = sum(e["size"] for e in index)
    print(f"{len(index)} overlays, {total / 1048576:.1f} MB -> {a.out}")


# ----------------------------------------------------------------------------- recomp

JR_RA = 0x03E00008


class Overlay:
    def __init__(self, path):
        self.path = path
        self.blob = open(path, "rb").read()
        if self.blob[:4] != b"MWo3":
            raise ValueError(f"{path}: not an MWo3 overlay")
        (_, self.ov_id, self.load, self.text, self.data, self.bss,
         self.ctors, self.dtors) = struct.unpack_from("<8I", self.blob, 0)
        self.name = os.path.splitext(os.path.basename(path))[0].upper()
        self.ident = "".join(c if c.isalnum() else "_" for c in self.name)
        self.lo = self.load + 0x40
        self.hi = self.lo + self.text
        if 0x40 + self.text + self.data != len(self.blob):
            raise ValueError(f"{path}: header sizes don't add up")

    def word(self, va):
        o = va - self.load
        if 0 <= o <= len(self.blob) - 4:
            return struct.unpack_from("<I", self.blob, o)[0]
        return None

    def crc32(self):
        import zlib
        return zlib.crc32(self.blob) & 0xFFFFFFFF


def is_prologue(w):
    return w is not None and (w >> 16) == 0x27BD and (w & 0x8000)


def looks_like_start(ov, t):
    """A data pointer / outside call may be a function start, or a jump-table case label."""
    if t % 8 or not (ov.lo <= t < ov.hi):
        return False
    if is_prologue(ov.word(t)):
        return True                    # `addiu sp, sp, -N` is never a live delay slot
    if in_delay_slot(ov.word, t):
        return False                   # e.g. the `addiu sp, sp, N` after `jr ra`
    prev1, prev2 = ov.word(t - 4), ov.word(t - 8)
    if prev2 == JR_RA or (prev1 == 0 and prev2 in (0, JR_RA)) or t == ov.lo:
        return True
    # right after an unconditional j/jr + delay slot (+ nop padding)
    return after_unconditional(ov.word, t)


def main_elf_refs(elf_path):
    """Overlay-range addresses the main ELF calls (jal) or stores as pointers."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from rd_elf import Elf, code_ranges
    e = Elf(elf_path)
    blocks, (lo, hi) = code_ranges(e)
    incode = lambda va: any(a <= va < b for a, b in blocks)
    calls, ptrs = set(), set()
    for va in range(lo, hi, 4):
        w = e.read32(va)
        if w is None:
            continue
        if incode(va):
            if (w >> 26) == 3:
                t = (va & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if t >= 0x700000:
                    calls.add(t)
        elif 0x700000 <= w < 0x2000000 and w % 4 == 0:
            ptrs.add(w)
    return calls, ptrs


def overlay_main_refs(overlay_dir, lo, hi):
    """Main-ELF code addresses the overlays reach: jal/j targets and data words.

    Functions only overlays call (small getters/setters after a `jr ra`, e.g.
    0x29E150 called by TITLE) are invisible to Ghidra and the main-ELF pointer
    pass, so the main recompile has no entry for them and the overlay stops with
    `guest-branch:missing-target`. Returns (calls, ptrs): sets of addresses.
    """
    import glob
    calls, ptrs = set(), set()
    for path in sorted(glob.glob(os.path.join(overlay_dir, "*.BIN"))):
        ov = Overlay(path)
        for va in range(ov.lo, ov.hi, 4):
            w = ov.word(va)
            if w and (w >> 26) in (2, 3):
                t = (va & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if lo <= t < hi:
                    calls.add(t)
        for o in range(0x40 + ov.text, len(ov.blob) - 3, 4):
            v = struct.unpack_from("<I", ov.blob, o)[0]
            if lo <= v < hi and v % 4 == 0:
                ptrs.add(v)
    return calls, ptrs


def discover(ov, main_calls, main_ptrs):
    """Function starts for one overlay: returns (sorted starts, stats)."""
    starts = set()
    t = ov.lo
    while t < ov.hi and ov.word(t) == 0:          # leading nop padding
        t += 4
    if t < ov.hi:
        starts.add(t)
    prologues = {va for va in range(ov.lo, ov.hi, 4) if is_prologue(ov.word(va))}
    calls = set()
    for va in range(ov.lo, ov.hi, 4):
        w = ov.word(va)
        if (w >> 26) == 3:
            tgt = (va & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            if ov.lo <= tgt < ov.hi:
                calls.add(tgt)
    ptrs = set()
    for o in range(0x40 + ov.text, len(ov.blob) - 3, 4):
        v = struct.unpack_from("<I", ov.blob, o)[0]
        if ov.lo <= v < ov.hi and looks_like_start(ov, v):
            ptrs.add(v)
    for va in range(ov.lo, ov.hi, 4):          # addresses built in code: lui rX + addiu/ori rX
        w = ov.word(va)
        op = w >> 26
        if op not in (0x09, 0x0D):
            continue
        rs, imm = (w >> 21) & 31, w & 0xFFFF
        for back in range(va - 4, max(ov.lo, va - 40) - 4, -4):
            x = ov.word(back)
            if x is not None and (x >> 26) == 0x0F and ((x >> 16) & 31) == rs:
                v = (((x & 0xFFFF) << 16) + (((imm ^ 0x8000) - 0x8000) if op == 0x09 else imm)) & 0xFFFFFFFF
                if ov.lo <= v < ov.hi and looks_like_start(ov, v):
                    ptrs.add(v)
                break
    outside = {v for v in (main_calls | main_ptrs) if ov.lo <= v < ov.hi}
    outside_ok = {v for v in outside if looks_like_start(ov, v)}
    starts |= prologues | calls | ptrs | outside_ok
    split = {v for v in starts if in_delay_slot(ov.word, v) and not is_prologue(ov.word(v))}
    starts -= split                    # a start inside a delay slot would cut the jump from it
    stats = dict(prologues=len(prologues), calls=len(calls), data_ptrs=len(ptrs),
                 main_refs=len(outside), main_refs_used=len(outside_ok), delay_slot_dropped=len(split))
    return sorted(starts), stats


def build_elf(ov, out, entry=None):
    """Wrap an overlay in a minimal ELF at its load address for ps2_recomp."""
    off_blob = 0x1000
    shstr = b"\0.text\0.data\0.bss\0.shstrtab\0"
    body = bytearray(off_blob) + ov.blob
    while len(body) % 4:
        body.append(0)
    off_shstr = len(body)
    body += shstr
    while len(body) % 4:
        body.append(0)
    off_sh = len(body)

    def sh(name, typ, flags, addr, off, size, align):
        return struct.pack("<10I", name, typ, flags, addr, off, size, 0, 0, align, 0)
    secs = [sh(0, 0, 0, 0, 0, 0, 0),
            sh(1, 1, 6, ov.lo, off_blob + 0x40, ov.text, 16),
            sh(7, 1, 3, ov.hi, off_blob + 0x40 + ov.text, ov.data, 16),
            sh(13, 8, 3, ov.load + len(ov.blob), 0, ov.bss, 16),
            sh(18, 3, 0, 0, off_shstr, len(shstr), 1)]
    body += b"".join(secs)
    eh = struct.pack("<16sHHIIIIIHHHHHH", b"\x7fELF\x01\x01\x01" + b"\0" * 9, 2, 8, 1, entry or ov.lo, 0x34, off_sh,
                     0x20924001, 0x34, 0x20, 1, 0x28, len(secs), 4)
    ph = struct.pack("<8I", 1, off_blob, ov.load, ov.load, len(ov.blob), len(ov.blob) + ov.bss, 7, 16)
    body[0:len(eh)] = eh
    body[0x34:0x34 + len(ph)] = ph
    with open(out, "wb") as f:
        f.write(body)


TABLE_LINE = None


def postprocess(ov, gen_dir, runner_dir):
    """Rename an overlay's generated functions to ov_<NAME>_..., flatten the files into
    the runner dir and return [(guest_addr, c_name)] for its dispatch table."""
    import re
    hdr = os.path.join(gen_dir, "ps2_recompiled_functions.h")
    with open(hdr, encoding="utf-8") as f:
        header = f.read()
    names = sorted(set(re.findall(r"^void (\w+)\(uint8_t\* rdram", header, re.M)), key=len, reverse=True)
    prefix = f"ov_{ov.ident}_"
    ren = {n: (n if n.startswith(prefix) else prefix + n) for n in names}
    pat = re.compile(r"\b(" + "|".join(map(re.escape, names)) + r")\b") if names else None

    def fix(text):
        return pat.sub(lambda m: ren[m.group(1)], text) if pat else text

    own_header = f"ov_{ov.ident}_functions.h"
    with open(os.path.join(runner_dir, own_header), "w", encoding="utf-8") as f:
        f.write(f"// generated by tools/rd_overlay.py for {ov.name}.BIN - do not edit\n#pragma once\n")
        f.write("".join(l + "\n" for l in fix(header).splitlines() if l.startswith("void ")))
    files = 0
    for fn in sorted(os.listdir(gen_dir)):
        if not fn.endswith(".cpp") or fn == "register_functions.cpp":
            continue
        with open(os.path.join(gen_dir, fn), encoding="utf-8") as f:
            src = f.read()
        src = src.replace("#include <ps2_recompiled_functions.h>", f'#include "{own_header}"')
        out_name = fn if fn.startswith(prefix) else prefix + fn
        with open(os.path.join(runner_dir, out_name), "w", encoding="utf-8") as f:
            f.write(fix(src))
        files += 1
    table = []
    with open(os.path.join(gen_dir, "register_functions.cpp"), encoding="utf-8") as f:
        for line in f:
            m = re.search(r"g_ps2RecompiledFunctionTable\[\d+\] = (\w+); // 0x([0-9a-fA-F]+)", line)
            if m:
                table.append((int(m.group(2), 16), ren.get(m.group(1), m.group(1))))
    return table, files


def write_tables(overlays, runner_dir):
    """rd_overlay_tables.cpp: one {addr, fn} array per overlay plus the index rd_game.cpp uses."""
    out = ["// generated by tools/rd_overlay.py - do not edit", '#include "ps2_runtime.h"',
           '#include "rd_overlay_tables.h"', ""]
    for ov, table, _ in overlays:
        if table:
            out.append(f'#include "ov_{ov.ident}_functions.h"')
    out.append("")
    for ov, table, _ in overlays:
        if not table:
            continue
        out.append(f"static const RdOverlayFunction k_{ov.ident}[] = {{")
        out += [f"    {{0x{a:08X}u, &{n}}}," for a, n in sorted(table)]
        out.append("};")
    out.append("")
    out.append("extern const RdOverlayInfo g_rdOverlays[] = {")
    for ov, table, _ in overlays:
        out.append(f'    {{"{ov.name}", 0x{ov.load:08X}u, {len(ov.blob)}u, {ov.bss}u, 0x{ov.crc32():08X}u, '
                   f"{('k_' + ov.ident) if table else 'nullptr'}, {len(table)}u}},")
    if not overlays:
        out.append('    {"", 0u, 0u, 0u, 0u, nullptr, 0u},')
    out.append("};")
    out.append(f"extern const uint32_t g_rdOverlayCount = {len(overlays)}u;")
    with open(os.path.join(runner_dir, "rd_overlay_tables.cpp"), "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


def cmd_recomp(a):
    import csv
    import glob
    import shutil
    import subprocess
    index_path = os.path.join(a.overlays, "index.json")
    if not os.path.exists(index_path):
        sys.exit(f"{index_path} missing - run: python rd.py overlays")
    work = os.path.abspath(a.out)
    runner = os.path.join(work, "runner")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(runner)
    main_calls, main_ptrs = main_elf_refs(a.elf)
    only = {x.strip().upper() for x in a.only.split(",")} if a.only else None
    done = []
    for path in sorted(glob.glob(os.path.join(a.overlays, "*.BIN"))):
        ov = Overlay(path)
        if only and ov.name not in only:
            continue
        d = os.path.join(work, ov.ident)
        gen = os.path.join(d, "gen")
        os.makedirs(gen)
        starts, stats = discover(ov, main_calls, main_ptrs)
        if not starts:
            done.append((ov, [], 0))   # data-only overlay: listed so the loader hook recognises it
            print(f"  {ov.name:<16} 0x{ov.load:06X} data only - nothing to recompile")
            continue
        elf = os.path.join(d, ov.ident + ".elf")
        build_elf(ov, elf, starts[0])
        fcsv = os.path.join(d, "functions.csv")
        with open(fcsv, "w", newline="", encoding="utf-8") as f:
            w = csv.writer(f, lineterminator="\n")
            w.writerow(["Name", "Start", "End", "Size"])
            for i, s in enumerate(starts):
                e = starts[i + 1] if i + 1 < len(starts) else ov.hi
                w.writerow([f"ov_{ov.ident}_{s:08x}", f"0x{s:08X}", f"0x{e:08X}", e - s])
        cfg = os.path.join(d, "config.toml")
        with open(cfg, "w", encoding="utf-8") as f:
            f.write("[general]\n")
            for k, v in (("input", elf), ("output", gen), ("ghidra_output", fcsv)):
                f.write(f'{k} = "{os.path.abspath(v).replace(chr(92), "/")}"\n')
            f.write("single_file_output = false\npatch_syscalls = false\npatch_cop0 = true\n"
                    "patch_cache = true\nstubs = []\nskip = []\n")
        log = os.path.join(d, "recomp.log")
        with open(log, "w", encoding="utf-8") as lf:
            rc = subprocess.call([a.recompiler, cfg], stdout=lf, stderr=subprocess.STDOUT)
        if rc != 0:
            sys.exit(f"ps2_recomp failed for {ov.name} (see {log})")
        table, files = postprocess(ov, gen, runner)
        done.append((ov, table, files))
        print(f"  {ov.name:<16} 0x{ov.load:06X} code 0x{ov.text:05X}: {len(starts):4d} functions, "
              f"{len(table):5d} entries  (prologues {stats['prologues']}, calls {stats['calls']}, "
              f"data ptrs {stats['data_ptrs']}, main refs {stats['main_refs_used']}/{stats['main_refs']})")
    write_tables(done, runner)
    print(f"{sum(1 for _, t, _ in done if t)} overlays recompiled, {sum(1 for _, t, _ in done if not t)} data-only -> {runner} "
          f"({sum(f for _, _, f in done)} files, {sum(len(t) for _, t, _ in done)} table entries)")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("extract")
    e.add_argument("--root", required=True, help="folder with ROOT.FAT + ROOT.DAT (work/us)")
    e.add_argument("--out", required=True)
    e.set_defaults(fn=cmd_extract)
    r = sub.add_parser("recomp")
    r.add_argument("--overlays", required=True)
    r.add_argument("--elf", required=True)
    r.add_argument("--recompiler", required=True, help="path to ps2_recomp")
    r.add_argument("--out", required=True)
    r.add_argument("--only", help="comma list of overlay names (default: all)")
    r.set_defaults(fn=cmd_recomp)
    a = ap.parse_args(argv)
    a.fn(a)


if __name__ == "__main__":
    main()
