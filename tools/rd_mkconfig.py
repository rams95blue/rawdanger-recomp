#!/usr/bin/env python3
"""
rd_mkconfig.py - generate the ps2recomp config + game-module headers.

Inputs
  --elf      extracted boot ELF (e.g. work/us/SLUS_215.01)
  --base     optional TOML from Ghidra ExportPS2Functions.java or ps2_analyzer
             (stubs / skip / entry_points / jump_tables / mmio are kept)
  --csv      optional Ghidra function CSV (ghidra_output)
  --runtime  ps2recomp/ps2xRuntime (generated C++ goes to its src/runner)
  --code-patches  comma list of patch groups whose CODE sites get baked in
                  (default: groups marked "default": true in patchdb)

Outputs
  recomp/out/config.toml                for ps2_recomp
  recomp/game/rd_build_info.h           ELF name / serial / CRC32 / entry
  recomp/game/rd_patch_table.inc        DATA patch sites (toggled in rawdanger.ini)

Every patch site is checked against the ELF; mismatching sites are dropped.
"""
from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rd_elf import Elf, load_db, game_for_elf, verify_sites, code_ranges  # noqa: E402

try:
    import tomllib  # Python 3.11+
except ImportError:  # pragma: no cover
    tomllib = None

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))


# ----------------------------------------------------------------------------- tiny TOML writer

def _tval(v, indent=""):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        return repr(v)
    if isinstance(v, str):
        return '"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"'
    if isinstance(v, list):
        if not v:
            return "[]"
        if all(not isinstance(x, (dict, list)) for x in v) and len(v) <= 4:
            return "[" + ", ".join(_tval(x) for x in v) + "]"
        if all(isinstance(x, dict) for x in v):
            return "[\n" + ",\n".join(indent + "  " + _inline_table(x) for x in v) + "\n" + indent + "]"
        return "[\n" + ",\n".join(indent + "  " + _tval(x, indent + "  ") for x in v) + ",\n" + indent + "]"
    if isinstance(v, dict):
        return _inline_table(v)
    raise TypeError(type(v))


def _inline_table(d: dict) -> str:
    return "{ " + ", ".join(f"{k} = {_tval(x)}" for k, x in d.items()) + " }"


def dump_toml(doc: dict, header: str = "") -> str:
    out = [header] if header else []
    scalars = {k: v for k, v in doc.items() if not isinstance(v, dict)}
    for k, v in scalars.items():
        out.append(f"{k} = {_tval(v)}")
    for name, table in doc.items():
        if not isinstance(table, dict):
            continue
        _dump_table(out, name, table)
    return "\n".join(out) + "\n"


def _dump_table(out, name, table):
    out.append("")
    out.append(f"[{name}]")
    subs = []
    for k, v in table.items():
        if isinstance(v, dict):
            subs.append((k, v))
        else:
            out.append(f"{k} = {_tval(v)}")
    for k, v in subs:
        _dump_table(out, f"{name}.{k}", v)


# ----------------------------------------------------------------------------- SDK symbol merge

def selector_addr(sel):
    if isinstance(sel, str) and "@" in sel:
        try:
            return int(sel.rsplit("@", 1)[1], 0)
        except ValueError:
            return None
    return None


RECOMPILE_INSTEAD_OF_STUB = ("sceVu0",)


def merge_sdk_symbols(general, analyzer_toml):
    """Ghidra gives good function boundaries but can't name SDK code in a stripped
    ELF (it only spots syscall wrappers). ps2_analyzer matches the SCE library
    symbol database (sceSifInitRpc, sceCdRead, ...). Union both; on the same
    address the analyzer's database name wins."""
    if tomllib is None:
        sys.exit("Python 3.11+ is needed to read TOML")
    with open(analyzer_toml, "rb") as f:
        sdk = tomllib.load(f).get("general", {})
    stubs = {}
    for sel in general.get("stubs", []):
        stubs[selector_addr(sel) or sel] = sel
    before = len(stubs)
    replaced = 0
    for sel in sdk.get("stubs", []):
        key = selector_addr(sel) or sel
        if key in stubs and stubs[key] != sel:
            replaced += 1
        stubs[key] = sel
    # Library functions recompiled from the game's own code instead of replaced by
    # the runtime's C++ versions. ps2recomp's sceVu0* stubs are approximations
    # ("reconstructed from behavior"); sceVu0MulMatrix even multiplies in the wrong
    # order (libvu0: m0 = m2 * m1). In Raw Danger that gave in-game world matrices
    # with four identical rows and exploding geometry. The recompiler handles every
    # VU0 macro instruction libvu0 uses, so the original code is the exact version.
    kept = []
    recompiled = []
    for sel in stubs.values():
        name = sel.rsplit("@", 1)[0] if isinstance(sel, str) else ""
        # lstrip: the runtime binds "_sceVu0ecossin" (libvu0's internal register-level
        # helper: angle in and cos/sin out through VU0 registers) to its C-ABI
        # sceVu0ecossin(dst=a0, angle=f12), which breaks every RotMatrix*.
        if name.lstrip("_").startswith(RECOMPILE_INSTEAD_OF_STUB):
            recompiled.append(name)
        else:
            kept.append(sel)
    general["stubs"] = kept
    if recompiled:
        print(f"  recompiled from the game instead of runtime stubs: {len(recompiled)} ({', '.join(sorted(recompiled)[:4])}, ...)")
    eps = list(general.get("entry_points", []))
    have = set(map(str, eps))
    eps += [e for e in sdk.get("entry_points", []) if str(e) not in have]
    general["entry_points"] = eps
    print(f"SDK symbols merged from {os.path.basename(analyzer_toml)}: stubs {before} -> {len(general['stubs'])} "
          f"({replaced} renamed), entry_points -> {len(eps)}")


# ----------------------------------------------------------------------------- function map

def load_function_ranges(csv_path):
    """Ghidra ExportPS2Functions CSV: Name,Start,End,Size (hex or decimal)."""
    import csv
    ranges = []
    with open(csv_path, newline="", encoding="utf-8", errors="replace") as f:
        for row in csv.reader(f):
            if len(row) < 3 or row[0] == "Name":
                continue
            try:
                start, end = int(row[1], 16), int(row[2], 16)
            except ValueError:
                continue
            if end > start:
                ranges.append((start, end))
    ranges.sort()
    return ranges


def in_ranges(ranges, va):
    return any(lo <= va < hi for lo, hi in ranges)


def block_of(blocks, va):
    return next(((lo, hi) for lo, hi in blocks if lo <= va < hi), None)


def clean_function_map(csv_path, blocks, out_path, elf=None):
    """Keep only Ghidra entries that are aligned and start in real code; clamp ends.

    Bad Ghidra boundaries (functions spanning data, unaligned starts from a wrong
    ISA guess) make ps2recomp fail or emit giant functions. With `elf`, entries
    that start in the delay slot of the previous function's last branch (Ghidra
    makes `entry_` functions where hand-written code branches to a delay slot,
    e.g. 0x2B5E44, 0x2C62B0) are merged into that function: split there, the
    recompiler would drop the delay-slot instruction from the branch.
    Returns (kept, dropped_data, dropped_unaligned, clamped, merged).
    """
    import csv
    kept = dropped_data = dropped_unaligned = clamped = merged = 0
    rows = []
    with open(csv_path, newline="", encoding="utf-8", errors="replace") as src:
        for row in csv.reader(src):
            if len(row) < 4 or row[0] == "Name":
                continue
            try:
                start, end = int(row[1], 16), int(row[2], 16)
            except ValueError:
                continue
            if start % 4:
                dropped_unaligned += 1
                continue
            blk = block_of(blocks, start)
            if not blk:
                dropped_data += 1
                continue
            if end > blk[1]:
                end = blk[1]
                clamped += 1
            rows.append([row[0], start, end])
    if elf is not None:
        from rd_elf import in_delay_slot
        is_prologue = lambda w: w is not None and (w >> 16) == 0x27BD and (w & 0x8000)
        rows.sort(key=lambda r: r[1])
        out = []
        for r in rows:
            if (out and out[-1][2] == r[1] and r[2] > r[1] and in_delay_slot(elf.read32, r[1])
                    and not is_prologue(elf.read32(r[1]))):
                out[-1][2] = r[2]
                merged += 1
                continue
            out.append(r)
        rows = out
    with open(out_path, "w", newline="", encoding="utf-8") as dst:
        w = csv.writer(dst, lineterminator="\n")
        w.writerow(["Name", "Start", "End", "Size"])
        for name, start, end in rows:
            w.writerow([name, f"0x{start:08X}", f"0x{end:08X}", end - start])
            kept += 1
    return kept, dropped_data, dropped_unaligned, clamped, merged


def add_pointer_targets(elf, blocks, seg, csv_path, extra_starts=(), max_len=0x4000, forced=(), extra_ptrs=(),
                        jump_targets=()):
    """Add functions that are only reachable through code pointers (data words or lui+addiu in code).

    Ghidra finds functions by following calls; vtable methods, callback tables
    and small thunks (e.g. Raw Danger's 0x312940: `j 0x312720; nop`) are only
    referenced from data, so they never get recompiled and the runtime stops
    with `guest-branch:missing-target` on the first indirect call. This scans
    the data part of the load segment for aligned words, and code for addresses
    built with lui+addiu/ori (e.g. 0x182240, a thunk whose address the next thunk
    passes as a callback), that point into a gap between known functions, or into
    a known range right after an unconditional jump + delay slot (two tiny
    functions merged into one), and appends those as functions to csv_path
    (ending at the next known start). `forced` are addresses known to be called
    or jumped to (jal/j targets in the code overlays): they become functions even
    inside a known range, as long as they are not a delay slot. `extra_ptrs` are
    data words from the overlays, checked like the ELF's own data pointers.
    `jump_targets` are the ELF's own j/jal targets: a tail call (`j`) into a leaf
    function with no prologue (Raw Danger's 0x200310, reached only by `j` from
    0x1CE0D8) is otherwise merged into the function before it. They are accepted
    with the same rule as pointers (in a gap, or right after jump + delay slot).
    Returns (added, skipped_long).
    """
    import bisect
    import csv
    rows = []
    with open(csv_path, newline="", encoding="utf-8") as f:
        for r in csv.reader(f):
            if len(r) >= 3 and r[0] != "Name":
                rows.append((int(r[1], 16), int(r[2], 16)))
    rows.sort()
    starts = sorted({a for a, _ in rows} | set(extra_starts))
    first = rows[0][0] if rows else 0
    row_starts = [a for a, _ in rows]

    def covered(v):
        i = bisect.bisect_right(row_starts, v) - 1
        return i >= 0 and rows[i][0] <= v < rows[i][1]

    lo, hi = seg
    start_set = set(starts)

    from rd_elf import after_unconditional as _after, in_delay_slot

    def after_unconditional(v):
        """v follows `j/jr` + delay slot (+ nop padding): a function boundary even inside a merged range."""
        return _after(elf.read32, v)

    def candidates():
        for va in range(lo - lo % 4, hi, 4):          # code pointers stored in data
            if not in_ranges(blocks, va):
                yield elf.read32(va)
        for b0, b1 in blocks:                          # addresses built in code: lui rX + addiu/ori rX
            for va in range(b0, b1, 4):
                w = elf.read32(va)
                op = w >> 26 if w is not None else 0
                if op not in (0x09, 0x0D):
                    continue
                rs, imm = (w >> 21) & 31, w & 0xFFFF
                for back in range(va - 4, max(b0, va - 40) - 4, -4):
                    x = elf.read32(back)
                    if x is not None and (x >> 26) == 0x0F and ((x >> 16) & 31) == rs:
                        lo16 = ((imm ^ 0x8000) - 0x8000) if op == 0x09 else imm
                        yield (((x & 0xFFFF) << 16) + lo16) & 0xFFFFFFFF
                        break
        yield from extra_ptrs

    targets = set()
    for v in candidates():
        if not v or v % 8 or v < first or v in targets or v in start_set or not in_ranges(blocks, v):
            continue
        w0, w1 = elf.read32(v), elf.read32(v + 4)
        if not w0 and not w1:              # padding, not code
            continue
        if in_delay_slot(elf.read32, v):
            continue                       # never split a jump from its delay slot
        if covered(v) and not after_unconditional(v):
            continue                       # jump-table case label or other mid-function address
        targets.add(v)

    for v in jump_targets:                 # the ELF's own tail calls / calls
        if v % 4 or v < first or v in targets or v in start_set or not in_ranges(blocks, v):
            continue
        if in_delay_slot(elf.read32, v):
            continue
        if covered(v) and not after_unconditional(v):
            continue                       # ordinary jump inside a function
        targets.add(v)

    for v in forced:                       # called/jumped to from an overlay: certainly a function entry
        if v % 4 or v in start_set or v in targets or not in_ranges(blocks, v) or in_delay_slot(elf.read32, v):
            continue
        targets.add(v)

    all_starts = sorted(set(starts) | targets)
    added, too_long = [], []
    for t in sorted(targets):
        i = bisect.bisect_right(all_starts, t)
        end = all_starts[i] if i < len(all_starts) else hi
        blk = block_of(blocks, t)
        end = min(end, blk[1]) if blk else end
        if end - t > max_len:
            too_long.append(t)
            continue
        added.append((t, end))
    if added:
        with open(csv_path, "a", newline="", encoding="utf-8") as f:
            w = csv.writer(f, lineterminator="\n")
            for t, end in added:
                w.writerow([f"ptr_{t:08x}", f"0x{t:08X}", f"0x{end:08X}", end - t])
    return len(added), [f"0x{t:08X}" for t in too_long]


def selector_ok(sel, blocks):
    """'name@0xADDR' selectors: drop unaligned or data addresses; keep name-only ones."""
    if isinstance(sel, int):
        addr = sel
    elif isinstance(sel, str) and "@" in sel:
        try:
            addr = int(sel.rsplit("@", 1)[1], 0)
        except ValueError:
            return True
    else:
        return True
    return addr % 4 == 0 and in_ranges(blocks, addr)


# ----------------------------------------------------------------------------- main

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", required=True)
    ap.add_argument("--base")
    ap.add_argument("--csv")
    ap.add_argument("--runtime", default=os.path.join(ROOT, "ps2recomp", "ps2xRuntime"))
    ap.add_argument("--db", default=os.path.join(ROOT, "patches", "patchdb.json"))
    ap.add_argument("--code-patches", default=None, help="comma list of groups, or 'none'")
    ap.add_argument("--out", default=os.path.join(ROOT, "recomp", "out", "config.toml"))
    ap.add_argument("--game-dir", default=os.path.join(ROOT, "recomp", "game"))
    ap.add_argument("--single-file", action="store_true")
    ap.add_argument("--merge-sdk", help="TOML from ps2_analyzer: merge its SCE-symbol-database stubs/entry_points")
    ap.add_argument("--skip-file", help="extra skip selectors, one per line (rd.py writes work/data_skip.txt)")
    ap.add_argument("--overlays", help="extracted MWo3 overlays (work/overlays): their calls into the main ELF become functions")
    a = ap.parse_args(argv)

    elf = Elf(a.elf)
    db = load_db(a.db)
    serial, game = game_for_elf(db, a.elf)
    if a.code_patches is None:
        code_groups = {p["group"] for p in game["patches"] if p.get("default")}
    elif a.code_patches.strip().lower() == "none":
        code_groups = set()
    else:
        code_groups = {g.strip() for g in a.code_patches.split(",") if g.strip()}

    # base config
    doc: dict = {}
    if a.base:
        if tomllib is None:
            sys.exit("Python 3.11+ is needed to read --base TOML")
        with open(a.base, "rb") as f:
            doc = tomllib.load(f)
    doc.pop("ghidra_export", None)
    general = doc.setdefault("general", {})
    general["input"] = os.path.abspath(a.elf).replace("\\", "/")
    general["output"] = os.path.abspath(os.path.join(a.runtime, "src", "runner")).replace("\\", "/")
    if a.csv:
        general["ghidra_output"] = os.path.abspath(a.csv).replace("\\", "/")
    general.setdefault("single_file_output", bool(a.single_file))
    general.setdefault("patch_syscalls", False)
    general.setdefault("patch_cop0", True)
    general.setdefault("patch_cache", True)
    general.setdefault("stubs", [])
    general.setdefault("skip", [])
    if a.merge_sdk:
        merge_sdk_symbols(general, a.merge_sdk)
    if a.skip_file and os.path.exists(a.skip_file):
        with open(a.skip_file, encoding="utf-8") as f:
            extra = [l.strip() for l in f if l.strip() and not l.startswith("#")]
        have = set(general["skip"])
        general["skip"] += [x for x in extra if x not in have]
        print(f"skip list: +{len(extra)} data-region pseudo-functions from {os.path.basename(a.skip_file)}")

    blocks, _seg = code_ranges(elf)
    if blocks:
        print("code ranges: " + ", ".join(f"0x{lo:08X}-0x{hi:08X}" for lo, hi in blocks))
        for key in ("stubs", "untracked_stubs", "entry_points"):
            if isinstance(general.get(key), list):
                before = len(general[key])
                general[key] = [x for x in general[key] if selector_ok(x, blocks)]
                if before != len(general[key]):
                    print(f"  {key}: dropped {before - len(general[key])} entries in data / unaligned")
        if a.csv:
            cleaned = os.path.join(os.path.dirname(os.path.abspath(a.out)), "functions.clean.csv")
            kept, dd, du, cl, mg = clean_function_map(a.csv, blocks, cleaned, elf)
            general["ghidra_output"] = cleaned.replace("\\", "/")
            print(f"  Ghidra map: kept {kept}, dropped {dd} in data + {du} unaligned, clamped {cl} ends, "
                  f"merged {mg} delay-slot starts")
            sdk = []
            for key in ("stubs", "untracked_stubs", "entry_points"):
                for x in general.get(key, []) or []:
                    if isinstance(x, str) and "@" in x:
                        try:
                            sdk.append(int(x.rsplit("@", 1)[1], 0))
                        except ValueError:
                            pass
            ov_calls, ov_ptrs = set(), set()
            if a.overlays and os.path.isdir(a.overlays):
                from rd_overlay import overlay_main_refs
                ov_calls, ov_ptrs = overlay_main_refs(a.overlays, _seg[0], _seg[1])
                print(f"  overlay references into the main ELF: {len(ov_calls)} call/jump targets, {len(ov_ptrs)} data words")
            elf_jumps = set()
            for b0, b1 in blocks:              # the main ELF's own j/jal targets
                for va in range(b0, b1, 4):
                    w = elf.read32(va)
                    if w is not None and (w >> 26) in (2, 3):
                        elf_jumps.add(((va + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2))
            n, long_gaps = add_pointer_targets(elf, blocks, _seg, cleaned, sdk, forced=ov_calls, extra_ptrs=ov_ptrs,
                                               jump_targets=elf_jumps)
            print(f"  pointer-only functions added: {n}" +
                  (f" (skipped {len(long_gaps)} gaps over 16 KB: {', '.join(long_gaps[:8])})" if long_gaps else ""))

    patches_tbl = doc.setdefault("patches", {})
    instr = patches_tbl.setdefault("instructions", [])
    existing = {int(str(p["address"]), 0) for p in instr}

    data_rows = []
    report = []
    for p, s, addr, val, exp, kind, cur, status in verify_sites(elf, game):
        if kind == "unknown" and blocks:
            kind = "code" if in_ranges(blocks, addr) else "data"  # measured code ranges decide
        line = f"{p['group']:<16} 0x{addr:08X} {kind:<8} {status}"
        if status in ("MISMATCH", "NOT-IN-FILE"):
            report.append(line + "  -> DROPPED")
            continue
        if kind == "code":
            if p["group"] in code_groups and addr not in existing:
                instr.append({"address": f"0x{addr:08X}", "value": f"0x{val:08X}"})
                existing.add(addr)
                report.append(line + "  -> baked into config.toml")
            else:
                report.append(line + "  -> code patch not selected")
        elif kind == "unknown":
            # No section table: route both ways. ps2recomp only patches words it
            # decodes as instructions, and RAM writes to code bytes don't change
            # compiled code, so the wrong route is a harmless no-op.
            data_rows.append((p["group"], addr, val, exp))
            if p["group"] in code_groups and addr not in existing:
                instr.append({"address": f"0x{addr:08X}", "value": f"0x{val:08X}"})
                existing.add(addr)
                report.append(line + "  -> UNKNOWN code/data (no code ranges found): baked AND runtime; confirm in Ghidra")
            else:
                report.append(line + "  -> UNKNOWN code/data (no code ranges found): runtime data patch")
        else:  # data / bss
            data_rows.append((p["group"], addr, val, exp))
            report.append(line + "  -> runtime data patch (rawdanger.ini [patches])")
    if not instr:
        patches_tbl.pop("instructions")
    if not patches_tbl:
        doc.pop("patches")

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    header = (f"# Raw Danger! recomp config - generated by tools/rd_mkconfig.py\n"
              f"# {serial} {os.path.basename(a.elf)} CRC32 {elf.crc32:08X}\n"
              f"# Code patch groups baked in: {', '.join(sorted(code_groups)) or 'none'}\n")
    with open(a.out, "w", encoding="utf-8") as f:
        f.write(dump_toml(doc, header))

    os.makedirs(a.game_dir, exist_ok=True)
    with open(os.path.join(a.game_dir, "rd_build_info.h"), "w", encoding="utf-8") as f:
        f.write("#pragma once\n// Generated by tools/rd_mkconfig.py - do not edit.\n")
        f.write(f'#define RD_ELF_NAME "{os.path.basename(a.elf)}"\n')
        f.write(f'#define RD_SERIAL "{serial}"\n')
        f.write(f"#define RD_ELF_CRC32 0x{elf.crc32:08X}u\n")
        f.write(f"#define RD_ELF_ENTRY 0x{elf.entry:08X}u\n")
    with open(os.path.join(a.game_dir, "rd_patch_table.inc"), "w", encoding="utf-8") as f:
        f.write("// Generated by tools/rd_mkconfig.py - do not edit.\n")
        f.write("// { group, address, value, expected_original, has_expected }\n")
        for g, addr, val, exp in data_rows:
            f.write(f'{{"{g}", 0x{addr:08X}u, 0x{val:08X}u, 0x{(exp or 0):08X}u, {"true" if exp is not None else "false"}}},\n')

    print(f"{serial} CRC32 {elf.crc32:08X} entry 0x{elf.entry:08X}")
    for r in report:
        print("  " + r)
    print(f"wrote {a.out}")
    print(f"wrote {os.path.join(a.game_dir, 'rd_build_info.h')} and rd_patch_table.inc ({len(data_rows)} data sites)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
