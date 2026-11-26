#!/usr/bin/env python3
"""
rd.py - Raw Danger! PC recomp pipeline.

  python rd.py setup        clone ps2recomp (pinned), apply our runtime patch, build ps2_recomp/ps2_analyzer
  python rd.py extract      extract US + JP ISOs into work/us, work/jp
  python rd.py compare      measure US vs JP assets -> work/reports/report.md
  python rd.py analyze      ELF reports + patch-site verification -> work/reports/elf_*.md
  python rd.py ghidra       headless Ghidra import + analysis + function export -> work/ghidra/
  python rd.py config       generate recomp/out/config.toml (uses work/ghidra/*.toml+csv if present)
  python rd.py recomp       apply patches, rebuild ps2_recomp if needed, run it, copy the game module into the runner
  python rd.py overlays     extract + recompile the 79 MWo3 code overlays (work/overlays -> runner)
  python rd.py build        build ps2EntryRunner (Release)
  python rd.py voicepack    build the hybrid JP voice archive (work/us/voice_jp/STR/*)
  python rd.py demos        compare the Type-A / Type-B demo discs with retail -> work/reports/demos.md
  python rd.py engine-check what changed in ps2-native-recomp since our fork, sorted by what helps Raw Danger
  python rd.py ra-logic     RetroAchievements' set (achievements/ra/5616.json, kept offline) as text: 5616_logic.txt
  python rd.py ra-port      the same set moved to the US program's addresses: achievements/ra/5616.us.json
                            -> work/reports/engine_updates.md (RawDangerPC.exe shows the same at start)
  python rd.py run          launch the recompiled game
  python rd.py all          setup -> extract -> compare -> analyze -> config -> recomp -> build

Defaults assume this layout (yours):
  RAW DANGER DECOMP/
    GAME US/Raw Danger (USA).iso
    GAME JP/Zettai Zetsumei Toshi 2 - Itetsuita Kioku-tachi (Japan).iso
    rawdanger-recomp/   <- this folder
"""
from __future__ import annotations

import argparse
import glob
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(ROOT, "tools")
WORK = os.path.join(ROOT, "work")
PS2RECOMP = os.path.join(ROOT, "ps2recomp")
PS2RECOMP_URL = "https://github.com/ran-j/ps2recomp.git"
PS2RECOMP_PIN = "e5636981a1c9de368f6b71e819c3dc188dfedbf4"  # 2026-10-22, the commit this project was written against
RUNNER_DIR = os.path.join(PS2RECOMP, "ps2xRuntime", "src", "runner")
GAME_MODULE_DIR = os.path.join(ROOT, "recomp", "game")
CONFIG = os.path.join(ROOT, "recomp", "out", "config.toml")
US_ELF_NAME = "SLUS_215.01"
GENERATED_HEADERS = ("ps2_recompiled_functions.h", "ps2_recompiled_stubs.h")
DATA_SKIP = os.path.join(WORK, "data_skip.txt")
EXE = ".exe" if os.name == "nt" else ""


def sh(cmd, cwd=None, check=True):
    print("  $ " + " ".join(f'"{c}"' if " " in str(c) else str(c) for c in cmd), flush=True)
    r = subprocess.run([str(c) for c in cmd], cwd=cwd)
    if check and r.returncode != 0:
        sys.exit(f"command failed ({r.returncode})")
    return r.returncode


def py(script, *args):
    return sh([sys.executable, os.path.join(TOOLS, script), *args])


def find_iso(region_dir_name):
    base = os.path.dirname(ROOT)
    hits = [p for p in glob.glob(os.path.join(base, region_dir_name, "*.iso")) if "DEMO" not in p.upper()]
    return hits[0] if hits else None


def find_tool(name):
    pats = [os.path.join(PS2RECOMP, "out", "tools", "**", name + EXE)]
    for p in pats:
        hits = [h for h in glob.glob(p, recursive=True) if os.path.isfile(h)]
        if hits:
            return sorted(hits, key=lambda h: ("Release" not in h, h))[0]
    sys.exit(f"{name} not built yet - run: python rd.py setup")


def need(cmd):
    if not shutil.which(cmd):
        sys.exit(f"'{cmd}' not found on PATH")


# ----------------------------------------------------------------------------- steps

def apply_patches():
    """Bring ps2recomp to: pinned commit + patches/ps2recomp/*.patch, in order.

    Patches touch overlapping lines (0003 edits code 0001 added), so testing each
    one alone for "already applied" is unreliable. Instead a fingerprint of the
    whole patch set is stored in ps2recomp/.rd_patch_stamp; when it changes, the
    files the patches touch are restored from the pinned commit and every patch
    is re-applied in order. Generated recompiler output is never touched.
    """
    import hashlib
    patches = sorted(glob.glob(os.path.join(ROOT, "patches", "ps2recomp", "*.patch")))
    digest = hashlib.sha1()
    for p in patches:
        digest.update(os.path.basename(p).encode())
        with open(p, "rb") as f:
            digest.update(f.read())
    stamp = PS2RECOMP_PIN + ":" + digest.hexdigest()
    marker = os.path.join(PS2RECOMP, ".rd_patch_stamp")
    if os.path.exists(marker) and open(marker, encoding="utf-8").read().strip() == stamp:
        print(f"  ps2recomp patches up to date ({len(patches)})")
        return
    touched, created = set(), set()
    for p in patches:
        with open(p, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
        for i, line in enumerate(lines):
            if line.startswith("+++ b/"):
                path = line[6:].strip()
                touched.add(path)
                if i >= 2 and lines[i - 1].startswith("--- /dev/null"):
                    created.add(path)
    tracked = [t for t in sorted(touched - created)
               if subprocess.run(["git", "cat-file", "-e", f"{PS2RECOMP_PIN}:{t}"], cwd=PS2RECOMP,
                                 capture_output=True).returncode == 0]
    print(f"  restoring {len(tracked)} file(s) from the pinned commit, re-applying {len(patches)} patch(es)")
    if tracked:
        sh(["git", "checkout", PS2RECOMP_PIN, "--", *tracked], cwd=PS2RECOMP)
    for c in created:
        path = os.path.join(PS2RECOMP, c.replace("/", os.sep))
        if os.path.exists(path):
            os.remove(path)
    for p in patches:
        sh(["git", "apply", "--whitespace=nowarn", p], cwd=PS2RECOMP)
    with open(marker, "w", encoding="utf-8") as f:
        f.write(stamp + "\n")
    print("  ps2recomp patches applied")


def step_setup(a):
    need("git")
    need("cmake")
    if not os.path.isdir(os.path.join(PS2RECOMP, ".git")):
        sh(["git", "clone", "--recurse-submodules", PS2RECOMP_URL, PS2RECOMP])
    sh(["git", "fetch", "--depth", "1", "origin", PS2RECOMP_PIN], cwd=PS2RECOMP, check=False)
    sh(["git", "checkout", PS2RECOMP_PIN], cwd=PS2RECOMP)
    sh(["git", "submodule", "update", "--init", "--recursive"], cwd=PS2RECOMP)
    apply_patches()
    build = os.path.join(PS2RECOMP, "out", "tools")
    sh(["cmake", "-S", PS2RECOMP, "-B", build, "-DCMAKE_BUILD_TYPE=Release",
        "-DPS2X_BUILD_RUNTIME=OFF", "-DPS2X_BUILD_TEST=OFF", "-DPS2X_BUILD_STUDIO=OFF"])
    sh(["cmake", "--build", build, "--config", "Release", "--parallel"])
    print("setup done: ps2_recomp =", find_tool("ps2_recomp"))


def step_extract(a):
    us = a.us_iso or find_iso("GAME US")
    jp = a.jp_iso or find_iso("GAME JP")
    if not us:
        sys.exit("US ISO not found - pass --us-iso")
    for iso, out in ((us, "us"), (jp, "jp")):
        if not iso:
            print("JP ISO not found - skipping (pass --jp-iso)")
            continue
        dst = os.path.join(WORK, out)
        if os.path.exists(os.path.join(dst, "_rd_manifest.json")) and not a.force:
            print(f"{dst} already extracted (use --force to redo)")
            continue
        py("rd_iso.py", "info", iso)
        py("rd_iso.py", "extract", iso, dst)
    with open(os.path.join(WORK, "iso_paths.txt"), "w", encoding="utf-8") as f:
        f.write(f"us={os.path.abspath(us)}\n")
        if jp:
            f.write(f"jp={os.path.abspath(jp)}\n")


def iso_path(region):
    p = os.path.join(WORK, "iso_paths.txt")
    if os.path.exists(p):
        for line in open(p, encoding="utf-8"):
            k, _, v = line.strip().partition("=")
            if k == region:
                return v
    return find_iso("GAME US" if region == "us" else "GAME JP")


def step_compare(a):
    py("rd_scan.py", "all", os.path.join(WORK, "us"), os.path.join(WORK, "jp"), "-o", os.path.join(WORK, "reports"))


def step_analyze(a):
    rep = os.path.join(WORK, "reports")
    os.makedirs(rep, exist_ok=True)
    for region in ("us", "jp"):
        root = os.path.join(WORK, region)
        man = os.path.join(root, "_rd_manifest.json")
        if not os.path.exists(man):
            continue
        import json
        elf = json.load(open(man, encoding="utf-8")).get("boot_elf")
        elf_path = os.path.join(root, elf)
        py("rd_elf.py", "info", elf_path, "--manifest", man, "-o", os.path.join(rep, f"elf_{region}.md"))
        sh([sys.executable, os.path.join(TOOLS, "rd_elf.py"), "verify", elf_path], check=False)


R5900_LANGUAGE = "r5900:LE:32:default"  # from ghidra-emotionengine-reloaded's emotionengine.ldefs
GHIDRA_DIR = os.path.join(WORK, "ghidra")


def find_ghidra(a):
    """Locate analyzeHeadless next to this project (e.g. RAW DANGER DECOMP/gihdra+ps2engine/...)."""
    name = "analyzeHeadless.bat" if os.name == "nt" else "analyzeHeadless"
    cands = []
    if a.ghidra:
        cands += [os.path.join(a.ghidra, "support", name)]
        cands += glob.glob(os.path.join(a.ghidra, "**", "support", name), recursive=True)
    if os.environ.get("GHIDRA_INSTALL_DIR"):
        cands.append(os.path.join(os.environ["GHIDRA_INSTALL_DIR"], "support", name))
    parent = os.path.dirname(ROOT)
    for depth in range(1, 5):
        cands += glob.glob(os.path.join(parent, *(["*"] * depth), "support", name))
    cands = [c for c in cands if os.path.isfile(c)]
    if not cands:
        sys.exit("analyzeHeadless not found - pass --ghidra <ghidra install folder> or set GHIDRA_INSTALL_DIR")
    cands.sort(key=lambda p: os.path.basename(os.path.dirname(os.path.dirname(p))), reverse=True)  # newest version first
    return cands[0]


def check_ps2_extension(headless):
    """Warn if ghidra-emotionengine-reloaded isn't installed for this Ghidra version."""
    install = os.path.dirname(os.path.dirname(headless))
    ver = os.path.basename(install)                      # ghidra_12.2_PUBLIC
    places = [os.path.join(install, "Ghidra", "Extensions")]
    if os.name == "nt" and os.environ.get("APPDATA"):
        places.append(os.path.join(os.environ["APPDATA"], "ghidra", ver, "Extensions"))
    places.append(os.path.join(os.path.expanduser("~"), ".config", "ghidra", ver, "Extensions"))
    places.append(os.path.join(os.path.expanduser("~"), ".ghidra", "." + ver, "Extensions"))
    for p in places:
        if os.path.isdir(p) and any("emotionengine" in d.lower() for d in os.listdir(p)):
            print(f"PS2 extension: {p}")
            return True
    print("WARNING: ghidra-emotionengine-reloaded not found for " + ver + " - R5900 language may be missing")
    return False


def step_ghidra(a):
    elf = os.path.join(WORK, "us", US_ELF_NAME)
    if not os.path.exists(elf):
        sys.exit("run extract first")
    headless = find_ghidra(a)
    print(f"Ghidra: {headless}")
    processor = a.processor or R5900_LANGUAGE
    if processor == R5900_LANGUAGE:
        check_ps2_extension(headless)
    # both scripts in one folder so Ghidra compiles them as one bundle
    scripts = os.path.join(GHIDRA_DIR, "scripts")
    os.makedirs(scripts, exist_ok=True)
    shutil.copy2(os.path.join(PS2RECOMP, "ps2xRecomp", "tools", "ghidra", "ExportPS2Functions.java"), scripts)
    shutil.copy2(os.path.join(TOOLS, "ghidra", "RdExportPS2.java"), scripts)
    project_dir = os.path.join(GHIDRA_DIR, "project")
    os.makedirs(project_dir, exist_ok=True)
    project = "RawDanger"
    out_toml = os.path.join(GHIDRA_DIR, US_ELF_NAME + ".toml")
    out_csv = os.path.join(GHIDRA_DIR, US_ELF_NAME + ".csv")
    log = os.path.join(GHIDRA_DIR, "headless.log")
    already = os.path.exists(os.path.join(project_dir, project + ".gpr"))
    cmd = [headless, project_dir, project]
    if already and not a.force:
        print("project exists: re-exporting without re-analysis (use --force to re-import)")
        cmd += ["-process", US_ELF_NAME, "-noanalysis"]
    else:
        print("importing + auto-analyzing (this ELF takes a while: expect 10-40 min)")
        cmd += ["-import", elf, "-overwrite", "-processor", processor, "-cspec", "default"]
    cmd += ["-scriptPath", scripts, "-postScript", "RdExportPS2.java", out_toml, out_csv,
            "-log", log, "-scriptlog", os.path.join(GHIDRA_DIR, "script.log")]
    for stale in (out_toml, out_csv):
        if os.path.exists(stale):
            os.remove(stale)
    sh(cmd, check=False)
    if not (os.path.exists(out_toml) and os.path.exists(out_csv)):
        sys.exit(f"Ghidra export failed - see {log} and {os.path.join(GHIDRA_DIR, 'script.log')}")
    nfun = sum(1 for _ in open(out_csv, encoding="utf-8", errors="replace")) - 1
    print(f"exported {nfun:,} functions/labels -> {out_toml} + .csv")
    print("next: python rd.py config && python rd.py recomp")


def step_config(a):
    elf = os.path.join(WORK, "us", US_ELF_NAME)
    if not os.path.exists(elf):
        sys.exit("run extract first")
    args = ["--elf", elf, "--runtime", os.path.join(PS2RECOMP, "ps2xRuntime")]
    gdir = GHIDRA_DIR
    base = a.base or next(iter(sorted(glob.glob(os.path.join(gdir, "*.toml")))), None)
    csv = a.csv or next(iter(sorted(glob.glob(os.path.join(gdir, "*.csv")))), None)
    if base:
        print(f"using Ghidra export: {base}")
    # ps2_analyzer identifies SCE SDK functions (sceSifInitRpc, sceCdRead, ...) from its
    # symbol database - always run it; it's the base without Ghidra, a merge source with it
    analyzer = os.path.join(WORK, "analyzer.toml")
    if not os.path.exists(analyzer) or os.path.getmtime(analyzer) < os.path.getmtime(elf) or a.force:
        sh([find_tool("ps2_analyzer"), elf, analyzer])
    if not base:
        print("No Ghidra export in work/ghidra/ - using ps2_analyzer as the base (Ghidra is better, see docs/WORKFLOW.md)")
        base = analyzer
    else:
        args += ["--merge-sdk", analyzer]
    args += ["--base", base]
    if csv:
        args += ["--csv", csv]
    if a.code_patches:
        args += ["--code-patches", a.code_patches]
    if os.path.exists(DATA_SKIP):
        args += ["--skip-file", DATA_SKIP]
    if os.path.exists(os.path.join(OVERLAY_DIR, "index.json")):
        args += ["--overlays", OVERLAY_DIR]   # functions only the overlays call
    else:
        print("note: no extracted overlays yet - run `rd overlays` then `rd config` again so the main ELF gets "
              "the functions only overlays call")
    py("rd_mkconfig.py", *args)


def step_sync(a=None):
    for gen in ("rd_build_info.h", "rd_patch_table.inc"):
        if not os.path.exists(os.path.join(GAME_MODULE_DIR, gen)):
            sys.exit(f"recomp/game/{gen} missing - run: python rd.py config")
    os.makedirs(RUNNER_DIR, exist_ok=True)
    for f in os.listdir(GAME_MODULE_DIR):
        shutil.copy2(os.path.join(GAME_MODULE_DIR, f), os.path.join(RUNNER_DIR, f))
    ov_runner = os.path.join(ROOT, "recomp", "out", "overlays", "runner")
    if os.path.isdir(ov_runner):   # recompiled overlays (rd overlays); replaces the empty default table
        files = os.listdir(ov_runner)
        for f in files:
            src, dst = os.path.join(ov_runner, f), os.path.join(RUNNER_DIR, f)
            if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(src) or f == "rd_overlay_tables.cpp":
                shutil.copy2(src, dst)
        print(f"overlays synced: {len(files)} files")
    print(f"game module synced -> {RUNNER_DIR}")


def run_recompiler():
    if os.path.isdir(RUNNER_DIR):
        for f in os.listdir(RUNNER_DIR):
            p = os.path.join(RUNNER_DIR, f)
            if os.path.isfile(p) and f.endswith((".cpp", ".h", ".inc")):
                os.remove(p)
    include_dir = os.path.join(PS2RECOMP, "ps2xRuntime", "include")
    for h in GENERATED_HEADERS:
        stale = os.path.join(include_dir, h)
        if os.path.exists(stale):
            os.remove(stale)
    sh([find_tool("ps2_recomp"), CONFIG])
    # generated code includes these as <...>; upstream keeps them in ps2xRuntime/include
    for h in GENERATED_HEADERS:
        src = os.path.join(RUNNER_DIR, h)
        if os.path.exists(src):
            shutil.move(src, os.path.join(include_dir, h))


def data_pseudo_functions():
    """Generated functions whose start lies in the ELF's data (see rd_elf.code_ranges)."""
    import re
    sys.path.insert(0, TOOLS)
    from rd_elf import Elf, code_ranges
    blocks, seg = code_ranges(Elf(os.path.join(WORK, "us", US_ELF_NAME)))
    header = os.path.join(PS2RECOMP, "ps2xRuntime", "include", "ps2_recompiled_functions.h")
    out = []
    with open(header, encoding="utf-8", errors="replace") as f:
        for m in re.finditer(r"void (\w+?)_0x([0-9a-fA-F]+)\(", f.read()):
            start = int(m.group(2), 16)
            if seg and seg[0] <= start < seg[1] and not any(lo <= start < hi for lo, hi in blocks):
                out.append(f"{m.group(1)}@0x{start:08X}")
    return out, blocks


def report_largest(n=5):
    files = [(os.path.getsize(os.path.join(RUNNER_DIR, f)), f) for f in os.listdir(RUNNER_DIR)
             if f.endswith(".cpp") and f != "register_functions.cpp" and not f.startswith("rd_")]
    files.sort(reverse=True)
    print("largest generated functions: " + ", ".join(f"{f} {s / 1e6:.1f} MB" for s, f in files[:n]))
    if files and files[0][0] > 4e6:
        print("  WARNING: a generated function over 4 MB usually means data was recompiled as code")


def rebuild_tools():
    """Rebuild ps2_recomp/ps2_analyzer when a patch changed the recompiler itself.

    Incremental: cmake only recompiles what the patches touched, and does nothing
    when the tools are already current.
    """
    build = os.path.join(PS2RECOMP, "out", "tools")
    if not os.path.isdir(build):
        sys.exit("recompiler tools not built - run: python rd.py setup")
    print("  making sure ps2_recomp is built from the patched source ...")
    sh(["cmake", "--build", build, "--config", "Release", "--parallel"])


def step_recomp(a):
    if not os.path.exists(CONFIG):
        sys.exit("run config first")
    if os.path.isdir(os.path.join(PS2RECOMP, ".git")):
        apply_patches()
        rebuild_tools()
    run_recompiler()
    data_funcs, blocks = data_pseudo_functions()
    if data_funcs:
        print(f"\n{len(data_funcs)} generated 'functions' start in DATA (real code: "
              + ", ".join(f"0x{lo:08X}-0x{hi:08X}" for lo, hi in blocks) + ")")
        old = set()
        if os.path.exists(DATA_SKIP):
            old = {l.strip() for l in open(DATA_SKIP, encoding="utf-8") if l.strip() and not l.startswith("#")}
        with open(DATA_SKIP, "w", encoding="utf-8") as f:
            f.write("# generated by rd.py recomp - data-region pseudo-functions to skip\n")
            f.write("\n".join(sorted(old | set(data_funcs))) + "\n")
        print("regenerating config with them skipped, then recompiling again ...")
        step_config(a)
        run_recompiler()
        left, _ = data_pseudo_functions()
        big = [x for x in left
               if os.path.getsize(os.path.join(RUNNER_DIR, f"{x.split('@')[0]}_0x{int(x.split('@')[1], 16):x}.cpp")) > 4096]
        print(f"data pseudo-functions now stubbed: {len(left) - len(big)}, still full-size: {len(big)}")
    report_largest()
    step_sync()


def step_build(a):
    if not os.path.isdir(os.path.join(PS2RECOMP, ".git")):
        sys.exit("ps2recomp missing - run: python rd.py setup")
    apply_patches()
    if os.path.isdir(RUNNER_DIR) and os.path.exists(os.path.join(GAME_MODULE_DIR, "rd_build_info.h")):
        step_sync()  # pick up game-module edits (recomp/game) without a re-recompile
    build = os.path.join(PS2RECOMP, "out", "runtime")
    cfg = "Debug" if a.debug else "Release"
    opts = ["-DPS2X_BUILD_RECOMP=OFF", "-DPS2X_BUILD_ANALYZER=OFF", "-DPS2X_BUILD_TEST=OFF",
            "-DPS2X_BUILD_STUDIO=OFF", f"-DCMAKE_BUILD_TYPE={cfg}",
            f"-DPS2X_ENABLE_AGRESSIVE_LOGS={'ON' if a.debug else 'OFF'}",
            f"-DPS2X_ENABLE_FFMPEG={'OFF' if a.no_ffmpeg else 'ON'}",
            f"-DPS2X_ENABLE_DEBUG_UI={'ON' if a.debug_ui else 'OFF'}",
            # /GL + /LTCG over ~1M recompiled instructions exhausts the compiler heap (C1002/LNK1257)
            f"-DPS2X_ENABLE_LTCG={'ON' if a.ltcg else 'OFF'}"]
    if a.unity_batch:
        opts.append(f"-DPS2X_RUNNER_UNITY_BUILD_BATCH_SIZE={a.unity_batch}")
    if os.name != "nt":
        opts.append("-DCMAKE_CXX_FLAGS=-msse4.2")  # GCC/Clang need SSE4 enabled for the runtime's vector paths
    sh(["cmake", "-S", PS2RECOMP, "-B", build, *opts])
    jobs = ["--parallel", str(a.jobs)] if a.jobs else ["--parallel"]
    sh(["cmake", "--build", build, "--config", cfg, "--target", "ps2EntryRunner", *jobs])
    print("built:", find_runner())


def find_runner():
    hits = [h for h in glob.glob(os.path.join(PS2RECOMP, "out", "runtime", "**", "ps2EntryRunner" + EXE), recursive=True)
            if os.path.isfile(h)]
    if not hits:
        sys.exit("ps2EntryRunner not built - run: python rd.py build")
    return sorted(hits, key=lambda h: ("Release" not in h, h))[0]


OVERLAY_DIR = os.path.join(WORK, "overlays")
OVERLAY_OUT = os.path.join(ROOT, "recomp", "out", "overlays")


def step_overlays(a):
    """Extract the MWo3 code overlays from ROOT.DAT and recompile them for the runner."""
    if a.force or not os.path.exists(os.path.join(OVERLAY_DIR, "index.json")):
        py("rd_overlay.py", "extract", "--root", os.path.join(WORK, "us"), "--out", OVERLAY_DIR)
    args = ["recomp", "--overlays", OVERLAY_DIR, "--elf", os.path.join(WORK, "us", US_ELF_NAME),
            "--recompiler", find_tool("ps2_recomp"), "--out", OVERLAY_OUT]
    if a.only:
        args += ["--only", a.only]
    py("rd_overlay.py", *args)
    if os.path.isdir(RUNNER_DIR):
        step_sync()


def step_voicepack(a):
    if not a.paths and not a.candidates:
        # default: hybrid voice archive from the real STR.FAT/STR.DAT layout
        extra = ["--mute-us-only"] if a.mute_us_only else []
        py("rd_strpack.py", "--us", os.path.join(WORK, "us"), "--jp", os.path.join(WORK, "jp"),
           "--out", os.path.join(WORK, "us", "voice_jp"), *extra)
        return
    args = ["--us", os.path.join(WORK, "us"), "--jp", os.path.join(WORK, "jp"),
            "--out", os.path.join(WORK, "us", "voice_jp")]
    if a.candidates:
        args += ["--from-candidates", os.path.join(WORK, "reports", "voice_candidates.json")]
    py("rd_voicepack.py", *args, *a.paths)


def step_run(a):
    game = os.path.join(WORK, "us")
    ini = os.path.join(game, "rawdanger.ini")
    if not os.path.exists(ini):
        iso = iso_path("us") or ""
        with open(os.path.join(ROOT, "recomp", "rawdanger.ini"), encoding="utf-8") as f:
            text = f.read().replace("image = Raw Danger (USA).iso", f"image = {iso}")
        with open(ini, "w", encoding="utf-8") as f:
            f.write(text)
        print(f"wrote {ini}")
    env_note = []
    if a.trace:
        os.environ["RD_TRACE"] = a.trace
        env_note.append(f"RD_TRACE={a.trace}")
    if a.threads:
        os.environ["RD_THREADS"] = str(a.threads)
        env_note.append(f"RD_THREADS={a.threads}")
    if a.gscap:
        os.environ["RD_GSCAP"] = str(a.gscap)
        env_note.append(f"RD_GSCAP={a.gscap}")
    if a.gscap_frames:
        os.environ["RD_GSCAP_FRAMES"] = str(a.gscap_frames)
        env_note.append(f"RD_GSCAP_FRAMES={a.gscap_frames}")
    if env_note:
        print("  " + " ".join(env_note))
    sh([find_runner(), os.path.join(game, US_ELF_NAME)], cwd=game, check=False)


def find_demo(kind):
    base = os.path.dirname(ROOT)
    for pat in ("*.cue", "*.iso", "*.bin"):
        for p in glob.glob(os.path.join(base, "*", "**", pat), recursive=True):
            name = os.path.basename(p).upper()
            if f"TYPE-{kind}" in name or f"TYPE {kind}" in name or f"TYPE_{kind}" in name:
                return p
    return None


def step_demos(a):
    args = []
    for flag, path in (("--us", a.us_iso or find_iso("GAME US")), ("--jp", a.jp_iso or find_iso("GAME JP")),
                       ("--demo-a", find_demo("A")), ("--demo-b", find_demo("B"))):
        if path:
            args += [flag, path]
        else:
            print(f"  ({flag[2:]} disc not found, skipped)")
    py("rd_demos.py", *args, "--out", os.path.join(WORK, "reports", "demos.md"))


def step_engine_check(a):
    py("rd_engine_check.py", "--out", os.path.join(WORK, "reports", "engine_updates.md"))


def step_ra_logic(a):
    py("rd_ra_logic.py")


def step_ra_port(a):
    py("rd_ra_port.py")


def step_framerate(a):
    """The frame-rate constants of the US program from the European one (docs/FRAMERATE.md): rd.py framerate US_ELF EU_ELF [30|60]."""
    if len(a.paths) < 2:
        sys.exit("rd.py framerate needs the US program (SLUS_215.01) and the European one (SLES_545.87), then 30 or 60")
    fps = a.paths[2] if len(a.paths) > 2 else "30"
    out = os.path.join(ROOT, "patches", "framerate", f"SLUS-21501_{fps}steps.generated.pnach")
    py("rd_framerate.py", "--us", a.paths[0], "--eu", a.paths[1], "--fps", fps, "--pnach", out)
    print(f"written {out}")


def step_eu(a):
    """What the Enhanced Edition takes from the European disc (docs/VERSION_DIFFERENCES.md): rd.py eu US.iso EU.iso [out.pnach]."""
    if len(a.paths) < 2:
        sys.exit("rd.py eu needs the US disc image and the European one (SLES-54587), then where to write the patch file (optional)")
    extra = ["--pnach", a.paths[2]] if len(a.paths) > 2 else []
    py("rd_eu.py", "--us", a.paths[0], "--eu", a.paths[1], *extra)


def step_all(a):
    for fn in (step_setup, step_extract, step_compare, step_analyze, step_config, step_recomp, step_build):
        print(f"\n==== {fn.__name__[5:]} ====")
        fn(a)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("step", choices=["setup", "extract", "compare", "analyze", "ghidra", "config", "recomp", "sync",
                                     "build", "overlays", "voicepack", "demos", "engine-check", "ra-logic", "ra-port", "run", "framerate", "eu", "all"])
    ap.add_argument("paths", nargs="*", help="voicepack: disc paths to swap")
    ap.add_argument("--us-iso"); ap.add_argument("--jp-iso"); ap.add_argument("--force", action="store_true")
    ap.add_argument("--only", help="overlays: comma list of overlay names to recompile (default: all)")
    ap.add_argument("--threads", type=int, help="run: log every guest thread's state every N seconds (RD_THREADS)")
    ap.add_argument("--gscap", type=float, help="run: start one GS capture N seconds after boot (sets RD_GSCAP)")
    ap.add_argument("--gscap-frames", type=int, help="run: frames per GS capture (RD_GSCAP_FRAMES); F9 again stops early")
    ap.add_argument("--trace", help="run: comma list of game function addresses to trace (RD_TRACE)")
    ap.add_argument("--ghidra", help="ghidra: Ghidra install folder (default: searched next to this project)")
    ap.add_argument("--processor", help="ghidra: language ID override (default r5900:LE:32:default)")
    ap.add_argument("--base", help="config: base TOML (default work/ghidra/*.toml, else ps2_analyzer)")
    ap.add_argument("--csv", help="config: Ghidra function CSV")
    ap.add_argument("--code-patches", help="config: comma list of patch groups to bake, or 'none'")
    ap.add_argument("--debug", action="store_true"); ap.add_argument("--debug-ui", action="store_true")
    ap.add_argument("--jobs", type=int, help="build: parallel compile jobs (lower it if the compiler runs out of memory)")
    ap.add_argument("--ltcg", action="store_true", help="build: whole-program optimization (needs huge RAM; off by default)")
    ap.add_argument("--unity-batch", type=int, help="build: generated files per unity TU (default 32; lower = less RAM per compile)")
    ap.add_argument("--no-ffmpeg", action="store_true", help="build: FMVs become stub frames, no FFmpeg DLLs")
    ap.add_argument("--candidates", action="store_true", help="voicepack: use every voice_candidates.json entry")
    ap.add_argument("--mute-us-only", action="store_true", help="voicepack: silence the 7 English-only lines")
    a = ap.parse_args(argv)
    os.makedirs(WORK, exist_ok=True)
    {"setup": step_setup, "extract": step_extract, "compare": step_compare, "analyze": step_analyze,
     "ghidra": step_ghidra,
     "config": step_config, "recomp": step_recomp, "sync": step_sync, "build": step_build,
     "overlays": step_overlays, "voicepack": step_voicepack, "demos": step_demos, "engine-check": step_engine_check, "ra-logic": step_ra_logic, "ra-port": step_ra_port, "run": step_run, "framerate": step_framerate, "eu": step_eu, "all": step_all}[a.step](a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
