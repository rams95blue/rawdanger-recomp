#!/usr/bin/env python3
"""
rd_scan.py - asset scanner + US/JP comparison for Raw Danger extracted discs.

  scan    <extracted_root> -o scan.json        scan one version
  compare <us_scan.json> <jp_scan.json> -o DIR write report.md + voice_candidates.json
  all     <us_root> <jp_root> -o DIR           scan both + compare

What it measures (stdlib only):
  * per file: size, CRC32, leading magic, compressibility (zlib ratio on a sample)
  * embedded signatures anywhere inside files/archives:
      VAGp  (SPU ADPCM sample: sample rate, length)
      SShd  (SS2/ADS stream: rate, channels, PCM vs ADPCM)
      IECSsreV (HD sound-bank header)
      TIM2  (texture: width, height, bpp, mipmaps)
      MPEG-1/2 sequence header (video: resolution, bitrate)
      MWo3  (Metrowerks overlay = CODE that must be recompiled too)
      \\x7fELF (IRX / ELF modules)
  * the comparison picks a winner per category from measured numbers, not guesses.
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import re
import statistics
import struct
import sys
import time
import zlib

CHUNK = 16 * 1024 * 1024
HEADER_LOOKAHEAD = 64 * 1024   # bytes a header parse may need past its signature
SAMPLE_FOR_RATIO = 256 * 1024
MAX_HITS_PER_FILE = 200_000

SIG = re.compile(rb"VAGp|SShd|IECSsreV|TIM2|\x00\x00\x01\xb3|MWo3|\x7fELF")

TIM2_TYPES = {1: ("RGB16", 16), 2: ("RGB24", 24), 3: ("RGB32", 32), 4: ("IDX4", 4), 5: ("IDX8", 8)}
MPEG_FPS = {1: 23.976, 2: 24, 3: 25, 4: 29.97, 5: 30, 6: 50, 7: 59.94, 8: 60}

KNOWN_MAGIC = {
    b"VAGp": "vag", b"SShd": "ss2-ads", b"IECS": "hd-bank", b"TIM2": "tim2",
    b"\x00\x00\x01\xba": "mpeg-ps(pss)", b"\x00\x00\x01\xb3": "mpeg-es",
    b"\x7fELF": "elf/irx", b"MWo3": "mwo3-overlay", b"RIFF": "riff",
    b"PK\x03\x04": "zip", b"MThd": "midi", b"SEQp": "seq", b"OggS": "ogg",
    b"\x89PNG": "png", b"BM": "bmp",
}


def magic_name(head: bytes) -> str:
    for m, n in KNOWN_MAGIC.items():
        if head.startswith(m):
            return n
    if head[:8] == b"IECSsreV":
        return "hd-bank"
    printable = all(32 <= c < 127 for c in head[:4])
    if printable and len(head) >= 4:
        return "magic:" + head[:4].decode("ascii")
    return "raw"


def parse_hit(buf: bytes, i: int):
    """Parse a signature at buf[i]. Return (kind, dict) or None if bogus."""
    tag = buf[i:i + 4]
    try:
        if tag == b"VAGp":
            ver, _, size, rate = struct.unpack_from(">IIII", buf, i + 4)
            if not (4000 <= rate <= 96000) or size == 0 or size > 64 * 1024 * 1024:
                return None
            name = buf[i + 0x20:i + 0x30].split(b"\0")[0].decode("ascii", "replace")
            secs = (size / 16 * 28) / rate
            return "vag", {"rate": rate, "bytes": size, "secs": round(secs, 3), "name": name, "ver": ver}
        if tag == b"SShd":
            hsz, typ, rate, ch, inter = struct.unpack_from("<IIIII", buf, i + 4)
            if hsz != 0x18 or not (4000 <= rate <= 96000) or not (1 <= ch <= 8):
                return None
            return "ss2", {"rate": rate, "channels": ch, "codec": "PCM16" if typ == 1 else "ADPCM", "interleave": inter}
        if tag == b"IECS":
            if buf[i:i + 8] != b"IECSsreV":
                return None
            return "hd", {}
        if tag == b"TIM2":
            ver, fmt, count = buf[i + 4], buf[i + 5], struct.unpack_from("<H", buf, i + 6)[0]
            if ver not in (3, 4) or count == 0 or count > 256:
                return None
            hdr = i + (0x80 if fmt == 1 else 0x10)
            pics = []
            for _ in range(min(count, 16)):
                if hdr + 0x30 > len(buf):
                    break
                total, clut, img, hsz, clutc, pfmt, mips, ctype, itype, w, h = struct.unpack_from("<IIIHHBBBBHH", buf, hdr)
                if itype not in TIM2_TYPES or not (1 <= w <= 2048) or not (1 <= h <= 2048) or total < hsz:
                    break
                pics.append({"w": w, "h": h, "fmt": TIM2_TYPES[itype][0], "bpp": TIM2_TYPES[itype][1], "mips": mips})
                hdr += total
            if not pics:
                return None
            return "tim2", {"pics": pics}
        if tag[:3] == b"\x00\x00\x01" and tag[3] == 0xB3:
            b = buf[i + 4:i + 12]
            if len(b) < 8:
                return None
            w = (b[0] << 4) | (b[1] >> 4)
            h = ((b[1] & 0x0F) << 8) | b[2]
            aspect, fps_code = b[3] >> 4, b[3] & 0x0F
            br = ((b[4] << 10) | (b[5] << 2) | (b[6] >> 6)) * 400
            if not (160 <= w <= 1920 and 112 <= h <= 1088 and w % 16 == 0 and h % 16 == 0
                    and fps_code in MPEG_FPS and aspect in (1, 2, 3, 4) and br > 0):
                return None
            return "mpeg", {"w": w, "h": h, "fps": MPEG_FPS[fps_code], "bitrate": br}
        if tag == b"MWo3":
            words = struct.unpack_from("<16I", buf, i)
            return "mwo3", {"header_words": [f"{x:08X}" for x in words],
                            "name": buf[i + 0x40:i + 0x60].split(b"\0")[0].decode("ascii", "replace")}
        if tag == b"\x7fELF":
            if buf[i + 4] != 1 or buf[i + 5] != 1:
                return None
            etype, mach = struct.unpack_from("<HH", buf, i + 16)
            if mach != 8:  # MIPS
                return None
            return "elf", {"type": "IRX" if etype == 0xFF80 else f"ET_{etype}"}
    except struct.error:
        return None
    return None


def scan_file(path: str, rel: str) -> dict:
    size = os.path.getsize(path)
    rec = {"path": rel, "size": size}
    crc = 0
    hits = collections.defaultdict(list)
    nhits = 0
    with open(path, "rb") as f:
        head = f.read(16)
        rec["magic"] = magic_name(head)
        rec["head_hex"] = head.hex()
        f.seek(0)
        # compressibility sample: start + middle
        sample = f.read(SAMPLE_FOR_RATIO // 2)
        if size > SAMPLE_FOR_RATIO:
            f.seek(size // 2)
            sample += f.read(SAMPLE_FOR_RATIO // 2)
        rec["zratio"] = round(len(zlib.compress(sample, 1)) / max(1, len(sample)), 3) if sample else 1.0
        f.seek(0)
        carry = b""          # unscanned bytes carried into the next window
        carry_pos = 0        # file offset of carry[0]
        while True:
            chunk = f.read(CHUNK)
            eof = len(chunk) < CHUNK
            if chunk:
                crc = zlib.crc32(chunk, crc)
            buf = carry + chunk
            buf_pos = carry_pos
            if not buf:
                break
            # only start matches where a full header still fits in this window,
            # unless this is the end of the file
            limit = len(buf) if eof else max(0, len(buf) - HEADER_LOOKAHEAD)
            for m in SIG.finditer(buf, 0, min(len(buf), limit + 16)):
                if m.start() >= limit:
                    break
                if nhits >= MAX_HITS_PER_FILE:
                    break
                r = parse_hit(buf, m.start())
                if r:
                    kind, d = r
                    d["off"] = buf_pos + m.start()
                    hits[kind].append(d)
                    nhits += 1
            if eof:
                break
            carry = buf[limit:]
            carry_pos = buf_pos + limit
    rec["crc32"] = f"{crc & 0xFFFFFFFF:08X}"
    if hits:
        rec["hits"] = {k: v for k, v in hits.items()}
    if nhits >= MAX_HITS_PER_FILE:
        rec["hits_truncated"] = True
    return rec


def scan_tree(root: str, label: str) -> dict:
    t0 = time.time()
    files = []
    for dp, _, fns in os.walk(root):
        for fn in fns:
            if fn.startswith("_rd_"):
                continue
            full = os.path.join(dp, fn)
            files.append((full, "/" + os.path.relpath(full, root).replace(os.sep, "/")))
    files.sort(key=lambda x: x[1])
    total = sum(os.path.getsize(f) for f, _ in files)
    out = {"label": label, "root": os.path.abspath(root), "files": []}
    manifest = os.path.join(root, "_rd_manifest.json")
    lba = {}
    if os.path.exists(manifest):
        with open(manifest, encoding="utf-8") as m:
            man = json.load(m)
        lba = {f["path"].upper(): f["lba"] for f in man.get("files", [])}
        out["serial"] = man.get("serial")
        out["boot_elf"] = man.get("boot_elf")
        out["boot_elf_crc32"] = man.get("boot_elf_crc32")
    done = 0
    for i, (full, rel) in enumerate(files, 1):
        rec = scan_file(full, rel)
        if rel.upper() in lba:
            rec["lba"] = lba[rel.upper()]
        out["files"].append(rec)
        done += rec["size"]
        sys.stdout.write(f"\r[{label}] {100.0 * done / max(1, total):5.1f}%  {i}/{len(files)}  {rel[:58]:<58}")
        sys.stdout.flush()
    print(f"\n[{label}] scanned {len(files)} files, {total:,} bytes in {time.time() - t0:.1f}s")
    return out


# ----------------------------------------------------------------------------- aggregation

def aggregate(scan: dict) -> dict:
    a = {
        "files": len(scan["files"]),
        "bytes": sum(f["size"] for f in scan["files"]),
        "by_magic": collections.Counter(),
        "bytes_by_magic": collections.Counter(),
        "vag_rates": collections.Counter(), "vag_secs": 0.0, "vag_count": 0,
        "ss2": collections.Counter(), "hd_banks": 0,
        "tim2_count": 0, "tim2_res": collections.Counter(), "tim2_fmt": collections.Counter(),
        "tim2_pixels": 0, "tim2_maxdim": 0,
        "mpeg": collections.Counter(), "mpeg_bitrates": [],
        "mwo3": 0, "irx": 0, "elf": 0,
        "zratio_weighted": 0.0,
    }
    zw = 0.0
    for f in scan["files"]:
        a["by_magic"][f["magic"]] += 1
        a["bytes_by_magic"][f["magic"]] += f["size"]
        zw += f["zratio"] * f["size"]
        h = f.get("hits", {})
        for v in h.get("vag", []):
            a["vag_rates"][v["rate"]] += 1
            a["vag_secs"] += v["secs"]
            a["vag_count"] += 1
        for s in h.get("ss2", []):
            a["ss2"][f'{s["rate"]}Hz/{s["channels"]}ch/{s["codec"]}'] += 1
        a["hd_banks"] += len(h.get("hd", []))
        for t in h.get("tim2", []):
            for p in t["pics"]:
                a["tim2_count"] += 1
                a["tim2_res"][f'{p["w"]}x{p["h"]}'] += 1
                a["tim2_fmt"][p["fmt"]] += 1
                a["tim2_pixels"] += p["w"] * p["h"]
                a["tim2_maxdim"] = max(a["tim2_maxdim"], p["w"], p["h"])
        for m in h.get("mpeg", []):
            a["mpeg"][f'{m["w"]}x{m["h"]}@{m["fps"]}'] += 1
            a["mpeg_bitrates"].append(m["bitrate"])
        a["mwo3"] += len(h.get("mwo3", []))
        for e in h.get("elf", []):
            if e["type"] == "IRX":
                a["irx"] += 1
            else:
                a["elf"] += 1
    a["zratio_weighted"] = round(zw / max(1, a["bytes"]), 3)
    return a


def fmt_counter(c: collections.Counter, n=12, unit="") -> str:
    if not c:
        return "—"
    return ", ".join(f"{k}{unit}: {v}" for k, v in c.most_common(n))


def median_rate(c: collections.Counter) -> float:
    vals = []
    for k, v in c.items():
        vals += [k] * min(v, 100000)
    return statistics.median(vals) if vals else 0


def human(n: float) -> str:
    for u in ("B", "KB", "MB", "GB"):
        if abs(n) < 1024:
            return f"{n:.1f} {u}"
        n /= 1024
    return f"{n:.1f} TB"


def verdict(us: dict, jp: dict) -> list[str]:
    v = []
    # audio
    mu, mj = median_rate(us["vag_rates"]), median_rate(jp["vag_rates"])
    if us["vag_count"] or jp["vag_count"]:
        if mu != mj:
            win = "US" if mu > mj else "JP"
            v.append(f"**SFX/voice samples (VAG):** {win} has the higher median sample rate "
                     f"({mu:.0f} Hz US vs {mj:.0f} Hz JP).")
        else:
            v.append(f"**SFX/voice samples (VAG):** same median sample rate ({mu:.0f} Hz). "
                     f"US {us['vag_count']} clips / {us['vag_secs'] / 60:.1f} min, "
                     f"JP {jp['vag_count']} clips / {jp['vag_secs'] / 60:.1f} min.")
    else:
        v.append("**SFX/voice samples:** no headered VAG found in either version - audio lives in "
                 "headerless banks/streams (see HD/SS2/unknown archives below). Compare archive sizes instead.")
    if us["ss2"] or jp["ss2"]:
        v.append(f"**Streams (SS2/ADS):** US {fmt_counter(us['ss2'], 4)} | JP {fmt_counter(jp['ss2'], 4)}")
    # textures
    if us["tim2_count"] or jp["tim2_count"]:
        avg_u = us["tim2_pixels"] / max(1, us["tim2_count"])
        avg_j = jp["tim2_pixels"] / max(1, jp["tim2_count"])
        win = "same (within 2%)" if abs(avg_u - avg_j) <= 0.02 * max(avg_u, avg_j) else ("US" if avg_u > avg_j else "JP")
        v.append(f"**Textures (TIM2):** avg {avg_u:.0f} px US vs {avg_j:.0f} px JP -> {win}. "
                 f"Count US {us['tim2_count']} / JP {jp['tim2_count']}, largest {us['tim2_maxdim']} vs {jp['tim2_maxdim']}. "
                 f"Formats US {fmt_counter(us['tim2_fmt'], 5)} | JP {fmt_counter(jp['tim2_fmt'], 5)}.")
    else:
        v.append("**Textures:** no TIM2 found - textures are in Irem's own container. "
                 "Per-file size/CRC diffs below show which texture archives changed.")
    # video
    if us["mpeg_bitrates"] or jp["mpeg_bitrates"]:
        bu = statistics.median(us["mpeg_bitrates"]) if us["mpeg_bitrates"] else 0
        bj = statistics.median(jp["mpeg_bitrates"]) if jp["mpeg_bitrates"] else 0
        v.append(f"**FMV (MPEG):** median declared bitrate {bu / 1e6:.2f} Mbps US vs {bj / 1e6:.2f} Mbps JP; "
                 f"modes US {fmt_counter(us['mpeg'], 3)} | JP {fmt_counter(jp['mpeg'], 3)}.")
    # compression
    v.append(f"**Overall compressibility** (zlib ratio, lower = more redundancy left = less compressed): "
             f"US {us['zratio_weighted']} vs JP {jp['zratio_weighted']}.")
    v.append(f"**Disc payload:** US {human(us['bytes'])} in {us['files']} files vs JP {human(jp['bytes'])} in {jp['files']} files.")
    return v


def compare(us_scan: dict, jp_scan: dict, outdir: str):
    os.makedirs(outdir, exist_ok=True)
    us, jp = aggregate(us_scan), aggregate(jp_scan)
    U = {f["path"].upper(): f for f in us_scan["files"]}
    J = {f["path"].upper(): f for f in jp_scan["files"]}
    common = sorted(set(U) & set(J))
    only_u = sorted(set(U) - set(J))
    only_j = sorted(set(J) - set(U))

    L = []
    w = L.append
    w("# Raw Danger! — US (SLUS-21501) vs JP (SLPS-25606) asset comparison\n")
    w(f"Generated {time.strftime('%Y-%m-%d %H:%M')} by rd_scan.py\n")
    w(f"- US root: `{us_scan['root']}`  ELF {us_scan.get('boot_elf')}  CRC32 {us_scan.get('boot_elf_crc32')}")
    w(f"- JP root: `{jp_scan['root']}`  ELF {jp_scan.get('boot_elf')}  CRC32 {jp_scan.get('boot_elf_crc32')}\n")
    w("## Verdict (measured)\n")
    for line in verdict(us, jp):
        w(f"- {line}")
    w("\n## Totals\n")
    w("| | US | JP |\n|---|---:|---:|")
    rows = [
        ("Files", us["files"], jp["files"]),
        ("Bytes", human(us["bytes"]), human(jp["bytes"])),
        ("VAG samples", us["vag_count"], jp["vag_count"]),
        ("VAG minutes", f"{us['vag_secs'] / 60:.1f}", f"{jp['vag_secs'] / 60:.1f}"),
        ("HD sound banks", us["hd_banks"], jp["hd_banks"]),
        ("TIM2 pictures", us["tim2_count"], jp["tim2_count"]),
        ("MPEG seq headers", sum(us["mpeg"].values()), sum(jp["mpeg"].values())),
        ("MWo3 overlays (code!)", us["mwo3"], jp["mwo3"]),
        ("IRX modules", us["irx"], jp["irx"]),
        ("zlib ratio (weighted)", us["zratio_weighted"], jp["zratio_weighted"]),
    ]
    for r in rows:
        w(f"| {r[0]} | {r[1]} | {r[2]} |")
    w("\n## Audio detail\n")
    w(f"- VAG rates US: {fmt_counter(us['vag_rates'], 10, ' Hz')}")
    w(f"- VAG rates JP: {fmt_counter(jp['vag_rates'], 10, ' Hz')}")
    w(f"- SS2 US: {fmt_counter(us['ss2'])}")
    w(f"- SS2 JP: {fmt_counter(jp['ss2'])}")
    w("\n## Texture detail\n")
    w(f"- TIM2 resolutions US: {fmt_counter(us['tim2_res'], 15)}")
    w(f"- TIM2 resolutions JP: {fmt_counter(jp['tim2_res'], 15)}")
    w("\n## Video detail\n")
    w(f"- US: {fmt_counter(us['mpeg'])}")
    w(f"- JP: {fmt_counter(jp['mpeg'])}")
    w("\n## File types (leading magic)\n")
    w("| magic | US files | US bytes | JP files | JP bytes |\n|---|---:|---:|---:|---:|")
    for k in sorted(set(us["by_magic"]) | set(jp["by_magic"]), key=lambda k: -(us["bytes_by_magic"][k] + jp["bytes_by_magic"][k])):
        w(f"| `{k}` | {us['by_magic'][k]} | {human(us['bytes_by_magic'][k])} | {jp['by_magic'][k]} | {human(jp['bytes_by_magic'][k])} |")

    w("\n## Per-file differences (common paths, biggest delta first)\n")
    w("| path | US size | JP size | Δ (JP-US) | same? | magic | US z | JP z |\n|---|---:|---:|---:|:-:|---|---:|---:|")
    diffs = []
    for k in common:
        u, j = U[k], J[k]
        diffs.append((abs(j["size"] - u["size"]), k, u, j))
    diffs.sort(key=lambda x: (-x[0], x[1]))
    for _, k, u, j in diffs:
        same = "✓" if u["crc32"] == j["crc32"] else "✗"
        w(f"| `{u['path']}` | {u['size']:,} | {j['size']:,} | {j['size'] - u['size']:+,} | {same} | {u['magic']} | {u['zratio']} | {j['zratio']} |")
    w(f"\n## Only in US ({len(only_u)})\n")
    for k in only_u:
        w(f"- `{U[k]['path']}` {U[k]['size']:,} B ({U[k]['magic']})")
    w(f"\n## Only in JP ({len(only_j)})\n")
    for k in only_j:
        w(f"- `{J[k]['path']}` {J[k]['size']:,} B ({J[k]['magic']})")

    # MWo3 headers - needed to recompile overlays
    w("\n## MWo3 overlay headers (raw words, for reverse-engineering the overlay format)\n")
    for label, scan in (("US", us_scan), ("JP", jp_scan)):
        n = 0
        for f in scan["files"]:
            for h in f.get("hits", {}).get("mwo3", []):
                if n < 40:
                    w(f"- {label} `{f['path']}` @0x{h['off']:X} name=`{h['name']}` words={' '.join(h['header_words'][:8])}")
                n += 1
        if n > 40:
            w(f"- … {n - 40} more in {label}")

    # voice candidates
    cands = []
    for _, k, u, j in diffs:
        uh, jh = u.get("hits", {}), j.get("hits", {})
        audioish = any(x in uh or x in jh for x in ("vag", "ss2", "hd"))
        name = k.lower()
        hint = any(s in name for s in ("voice", "vo", "snd", "sound", "se", "bgm", "stream", "str", "adx", "vag", "sd", "audio"))
        if u["crc32"] != j["crc32"] and (audioish or hint):
            cands.append({"path": u["path"], "us_size": u["size"], "jp_size": j["size"],
                          "us_lba": u.get("lba"), "jp_lba": j.get("lba"),
                          "us_vag": len(uh.get("vag", [])), "jp_vag": len(jh.get("vag", [])),
                          "us_hd": len(uh.get("hd", [])), "jp_hd": len(jh.get("hd", [])),
                          "size_fits_in_us_slot": j["size"] <= u["size"]})
    with open(os.path.join(outdir, "voice_candidates.json"), "w", encoding="utf-8") as f:
        json.dump(cands, f, indent=1)
    w(f"\n## Voice-swap candidates ({len(cands)}) — see voice_candidates.json\n")
    for c in cands[:60]:
        fit = "fits" if c["size_fits_in_us_slot"] else "LARGER than US slot"
        w(f"- `{c['path']}` US {c['us_size']:,} / JP {c['jp_size']:,} ({fit}); VAG {c['us_vag']}/{c['jp_vag']}, HD {c['us_hd']}/{c['jp_hd']}")

    with open(os.path.join(outdir, "report.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(L) + "\n")
    summary = {"us": {k: (dict(v) if isinstance(v, collections.Counter) else v) for k, v in us.items() if k != "mpeg_bitrates"},
               "jp": {k: (dict(v) if isinstance(v, collections.Counter) else v) for k, v in jp.items() if k != "mpeg_bitrates"},
               "verdict": verdict(us, jp)}
    with open(os.path.join(outdir, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=1, default=str)
    print(f"Report: {os.path.join(outdir, 'report.md')}")
    for line in verdict(us, jp):
        print(" -", line.replace("**", ""))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("scan"); p.add_argument("root"); p.add_argument("-o", "--out", required=True)
    p.add_argument("--label", default=None)
    p = sub.add_parser("compare"); p.add_argument("us"); p.add_argument("jp"); p.add_argument("-o", "--outdir", required=True)
    p = sub.add_parser("all"); p.add_argument("us_root"); p.add_argument("jp_root"); p.add_argument("-o", "--outdir", required=True)
    a = ap.parse_args(argv)
    if a.cmd == "scan":
        s = scan_tree(a.root, a.label or os.path.basename(os.path.normpath(a.root)))
        with open(a.out, "w", encoding="utf-8") as f:
            json.dump(s, f)
    elif a.cmd == "compare":
        with open(a.us, encoding="utf-8") as f:
            us = json.load(f)
        with open(a.jp, encoding="utf-8") as f:
            jp = json.load(f)
        compare(us, jp, a.outdir)
    else:
        os.makedirs(a.outdir, exist_ok=True)
        us = scan_tree(a.us_root, "US")
        with open(os.path.join(a.outdir, "scan_us.json"), "w", encoding="utf-8") as f:
            json.dump(us, f)
        jp = scan_tree(a.jp_root, "JP")
        with open(os.path.join(a.outdir, "scan_jp.json"), "w", encoding="utf-8") as f:
            json.dump(jp, f)
        compare(us, jp, a.outdir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
