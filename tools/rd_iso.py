#!/usr/bin/env python3
"""
rd_iso.py - pure-Python ISO9660 reader/extractor for PS2 DVD images.

No dependencies (stdlib only). Streams data, so multi-GB images are fine.

Commands
  info    <iso>                   volume info, SYSTEM.CNF, boot ELF + CRC32
  list    <iso> [--json]          every file with LBA, size
  extract <iso> <outdir> [--only PATTERN ...] [--no-manifest]
                                  extract the tree (+ manifest.json with LBAs)
  elf     <iso> <outdir>          extract SYSTEM.CNF + boot ELF only (fast)

The manifest records each file's LBA. Many PS2 games read data by raw sector
number instead of by file name, so the LBA layout of the original disc matters
for the recompiled build and for any voice/texture swap.
"""
from __future__ import annotations

import argparse
import fnmatch
import json
import os
import struct
import sys
import time
import zlib
from dataclasses import dataclass, asdict

SECTOR = 2048
COPY_CHUNK = 8 * 1024 * 1024


@dataclass
class IsoEntry:
    path: str          # "/DATA/SOUND.BIN" (no ;1)
    lba: int
    size: int
    is_dir: bool
    mtime: float = 0.0
    extents: list | None = None   # [(lba,size),...] for multi-extent files


def _both_le(buf: bytes, off: int) -> int:
    # ISO9660 "both-endian" 32-bit: LE first
    return struct.unpack_from("<I", buf, off)[0]


def _rec_time(b: bytes) -> float:
    try:
        y, mo, d, h, mi, s = b[0] + 1900, b[1], b[2], b[3], b[4], b[5]
        return time.mktime((y, mo, d, h, mi, s, 0, 0, -1))
    except Exception:
        return 0.0


class Iso9660:
    def __init__(self, path: str):
        self.path = path
        self.f = open(path, "rb")
        self.size = os.path.getsize(path)
        self._read_pvd()

    def close(self):
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def read_sectors(self, lba: int, count: int = 1) -> bytes:
        self.f.seek(lba * SECTOR)
        return self.f.read(count * SECTOR)

    def _read_pvd(self):
        lba = 16
        while True:
            d = self.read_sectors(lba)
            if len(d) < SECTOR:
                raise ValueError("No ISO9660 Primary Volume Descriptor found")
            if d[1:6] != b"CD001":
                raise ValueError(f"Not an ISO9660 image (no CD001 at sector {lba})")
            if d[0] == 1:
                break
            if d[0] == 255:
                raise ValueError("No Primary Volume Descriptor (terminator reached)")
            lba += 1
        self.pvd = d
        self.system_id = d[8:40].decode("ascii", "replace").strip()
        self.volume_id = d[40:72].decode("ascii", "replace").strip()
        self.volume_blocks = _both_le(d, 80)
        self.block_size = struct.unpack_from("<H", d, 128)[0]
        if self.block_size != SECTOR:
            raise ValueError(f"Unsupported logical block size {self.block_size}")
        root = d[156:156 + 34]
        self.root_lba = _both_le(root, 2)
        self.root_size = _both_le(root, 10)
        self.app_id = d[574:702].decode("ascii", "replace").strip()
        self.creation = d[813:830].decode("ascii", "replace")

    def _iter_dir(self, lba: int, size: int):
        data = self.read_sectors(lba, (size + SECTOR - 1) // SECTOR)[:size]
        off = 0
        while off < len(data):
            ln = data[off]
            if ln == 0:
                # records never cross sector boundaries -> skip to next sector
                off = (off // SECTOR + 1) * SECTOR
                continue
            rec = data[off:off + ln]
            off += ln
            name_len = rec[32]
            name = rec[33:33 + name_len]
            if name in (b"\x00", b"\x01"):
                continue
            flags = rec[25]
            yield {
                "name": name.decode("ascii", "replace").split(";")[0].rstrip("."),
                "lba": _both_le(rec, 2),
                "size": _both_le(rec, 10),
                "is_dir": bool(flags & 0x02),
                "multi": bool(flags & 0x80),
                "mtime": _rec_time(rec[18:25]),
            }

    def walk(self) -> list[IsoEntry]:
        out: list[IsoEntry] = []
        seen_dirs = set()
        stack = [("", self.root_lba, self.root_size)]
        while stack:
            prefix, lba, size = stack.pop()
            if lba in seen_dirs:
                continue
            seen_dirs.add(lba)
            pending_multi: IsoEntry | None = None
            for r in self._iter_dir(lba, size):
                p = f"{prefix}/{r['name']}"
                if r["is_dir"]:
                    out.append(IsoEntry(p, r["lba"], r["size"], True, r["mtime"]))
                    stack.append((p, r["lba"], r["size"]))
                    continue
                # multi-extent: consecutive records with the same name
                if pending_multi is not None and pending_multi.path == p:
                    pending_multi.extents.append((r["lba"], r["size"]))
                    pending_multi.size += r["size"]
                    if not r["multi"]:
                        pending_multi = None
                    continue
                e = IsoEntry(p, r["lba"], r["size"], False, r["mtime"])
                if r["multi"]:
                    e.extents = [(r["lba"], r["size"])]
                    pending_multi = e
                out.append(e)
        out.sort(key=lambda e: (e.lba, e.path))
        return out

    def find(self, path: str) -> IsoEntry | None:
        want = path.upper().lstrip("/")
        for e in self.walk():
            if e.path.upper().lstrip("/") == want:
                return e
        return None

    def read_file(self, e: IsoEntry) -> bytes:
        parts = []
        for lba, size in (e.extents or [(e.lba, e.size)]):
            self.f.seek(lba * SECTOR)
            parts.append(self.f.read(size))
        return b"".join(parts)

    def copy_file(self, e: IsoEntry, dst: str) -> int:
        crc = 0
        with open(dst, "wb") as o:
            for lba, size in (e.extents or [(e.lba, e.size)]):
                self.f.seek(lba * SECTOR)
                left = size
                while left:
                    chunk = self.f.read(min(COPY_CHUNK, left))
                    if not chunk:
                        raise IOError(f"Unexpected EOF reading {e.path}")
                    o.write(chunk)
                    crc = zlib.crc32(chunk, crc)
                    left -= len(chunk)
        if e.mtime:
            try:
                os.utime(dst, (e.mtime, e.mtime))
            except OSError:
                pass
        return crc & 0xFFFFFFFF


def parse_system_cnf(text: str) -> dict:
    out = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip().upper()] = v.strip()
    boot = out.get("BOOT2") or out.get("BOOT") or ""
    # cdrom0:\SLUS_215.01;1
    name = boot.split(":", 1)[-1].lstrip("\\/").split(";")[0]
    out["_BOOT_ELF"] = name.replace("\\", "/")
    return out


def serial_from_elf_name(name: str) -> str:
    # SLUS_215.01 -> SLUS-21501
    base = os.path.basename(name).upper()
    return base.replace("_", "-", 1).replace(".", "")


def cmd_info(a):
    with Iso9660(a.iso) as iso:
        files = [e for e in iso.walk() if not e.is_dir]
        print(f"Image          : {a.iso}")
        print(f"Image size     : {iso.size:,} bytes")
        print(f"System ID      : {iso.system_id}")
        print(f"Volume ID      : {iso.volume_id}")
        print(f"Volume blocks  : {iso.volume_blocks:,} ({iso.volume_blocks * SECTOR:,} bytes)")
        print(f"Created        : {iso.creation}")
        print(f"Files          : {len(files):,}  ({sum(e.size for e in files):,} bytes)")
        cnf = iso.find("SYSTEM.CNF")
        if not cnf:
            print("SYSTEM.CNF     : (missing)")
            return 1
        text = iso.read_file(cnf).decode("ascii", "replace")
        info = parse_system_cnf(text)
        print("SYSTEM.CNF     :")
        for line in text.strip().splitlines():
            print(f"    {line}")
        elf = iso.find(info["_BOOT_ELF"])
        if elf:
            data = iso.read_file(elf)
            print(f"Boot ELF       : {elf.path}  LBA {elf.lba}  {elf.size:,} bytes")
            print(f"Serial         : {serial_from_elf_name(elf.path)}")
            print(f"ELF CRC32 (file, ps2recomp override key): {zlib.crc32(data) & 0xFFFFFFFF:08X}")
        biggest = sorted(files, key=lambda e: -e.size)[:15]
        print("Largest files  :")
        for e in biggest:
            print(f"    {e.size:>14,}  LBA {e.lba:>8}  {e.path}")
    return 0


def cmd_list(a):
    with Iso9660(a.iso) as iso:
        ents = iso.walk()
        if a.json:
            json.dump([asdict(e) for e in ents], sys.stdout, indent=1)
            print()
        else:
            for e in ents:
                kind = "<DIR>" if e.is_dir else f"{e.size:,}"
                print(f"{e.lba:>8}  {kind:>14}  {e.path}")
    return 0


def _safe_join(root: str, iso_path: str) -> str:
    rel = iso_path.lstrip("/").replace("/", os.sep)
    out = os.path.normpath(os.path.join(root, rel))
    if not out.startswith(os.path.normpath(root)):
        raise ValueError(f"Refusing path outside output dir: {iso_path}")
    return out


def cmd_extract(a):
    os.makedirs(a.outdir, exist_ok=True)
    t0 = time.time()
    manifest = {"image": os.path.abspath(a.iso), "files": []}
    with Iso9660(a.iso) as iso:
        manifest["volume_id"] = iso.volume_id
        manifest["volume_blocks"] = iso.volume_blocks
        ents = iso.walk()
        files = [e for e in ents if not e.is_dir]
        if a.only:
            files = [e for e in files if any(fnmatch.fnmatch(e.path.upper(), p.upper()) for p in a.only)]
        total = sum(e.size for e in files)
        done = 0
        for e in ents:
            if e.is_dir and not a.only:
                os.makedirs(_safe_join(a.outdir, e.path), exist_ok=True)
        for i, e in enumerate(files, 1):
            dst = _safe_join(a.outdir, e.path)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            crc = iso.copy_file(e, dst)
            done += e.size
            manifest["files"].append({"path": e.path, "lba": e.lba, "size": e.size,
                                      "crc32": f"{crc:08X}", "extents": e.extents})
            pct = 100.0 * done / total if total else 100.0
            sys.stdout.write(f"\r[{pct:5.1f}%] {i}/{len(files)}  {e.path[:60]:<60}")
            sys.stdout.flush()
        print()
        cnf = iso.find("SYSTEM.CNF")
        if cnf:
            info = parse_system_cnf(iso.read_file(cnf).decode("ascii", "replace"))
            manifest["boot_elf"] = info["_BOOT_ELF"]
            manifest["serial"] = serial_from_elf_name(info["_BOOT_ELF"])
            for f in manifest["files"]:
                if f["path"].upper().lstrip("/") == info["_BOOT_ELF"].upper():
                    manifest["boot_elf_crc32"] = f["crc32"]
    if not a.no_manifest:
        with open(os.path.join(a.outdir, "_rd_manifest.json"), "w", encoding="utf-8") as m:
            json.dump(manifest, m, indent=1)
    dt = time.time() - t0
    print(f"Extracted {len(manifest['files'])} files, {done:,} bytes in {dt:.1f}s -> {a.outdir}")
    if "boot_elf" in manifest:
        print(f"Boot ELF: {manifest['boot_elf']}  serial {manifest['serial']}  CRC32 {manifest.get('boot_elf_crc32')}")
    return 0


def cmd_elf(a):
    a.only = ["/SYSTEM.CNF"]
    with Iso9660(a.iso) as iso:
        cnf = iso.find("SYSTEM.CNF")
        if cnf:
            info = parse_system_cnf(iso.read_file(cnf).decode("ascii", "replace"))
            a.only.append("/" + info["_BOOT_ELF"])
    a.no_manifest = True
    return cmd_extract(a)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info"); p.add_argument("iso"); p.set_defaults(fn=cmd_info)
    p = sub.add_parser("list"); p.add_argument("iso"); p.add_argument("--json", action="store_true"); p.set_defaults(fn=cmd_list)
    p = sub.add_parser("extract"); p.add_argument("iso"); p.add_argument("outdir")
    p.add_argument("--only", nargs="*", help="glob(s) on ISO paths, e.g. '/*.ELF' '*SOUND*'")
    p.add_argument("--no-manifest", action="store_true"); p.set_defaults(fn=cmd_extract)
    p = sub.add_parser("elf"); p.add_argument("iso"); p.add_argument("outdir"); p.set_defaults(fn=cmd_elf)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
