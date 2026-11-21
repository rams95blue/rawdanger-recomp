#!/usr/bin/env python3
"""rd_card.py - list PS2 memory card images (.ps2) and carry Raw Danger! saves between the US and the Japanese program.

  python tools/rd_card.py list CARD.ps2
  python tools/rd_card.py mirror --from A.ps2 --to B.ps2 --from-code BASLUS-21501 --to-code BISLPS-25606
  python tools/rd_card.py export --card CARD.ps2 --to SAVES --code BASLUS-21501
  python tools/rd_card.py build --from SAVES --card CARD.ps2 --code BASLUS-21501 [--other-code BISLPS-25606]
  python tools/rd_card.py selftest

Saves without the memory card (the launcher, v0.7.8): the saves are plain folders, one per save, in SAVES (the launcher's
"saves" folder): SAVES/BASLUS-21501/ (the system data), SAVES/BASLUS-21501000/ ... (the slots), each with the save's files
as they are on the card and _card.json (the card's directory entries: dates, modes, attributes). The card image the game
runs with is made from them before each start (build: a freshly formatted card with every save of this program; a slot of
the other program that is newer, or that this program does not have, is brought over under this program's name: the
language switch needs no step of its own) and written back after the game ends (export: only the saves that changed are
rewritten, each one whole into a new folder that then takes the old one's place; a save no longer on the card, deleted in
the game, goes to SAVES/_deleted/). A card image from before (the old <game>.ps2) is read once to bring its saves in.

mirror: every save folder of the first program on card A (BASLUS-21501 = the system data, BASLUS-21501000... = the slots) is
written to card B under the other program's name (BISLPS-25606, BISLPS-25606000...), with the files inside renamed the same
way. A folder already on B with that name is replaced; B's other slots stay. B is kept as B.bak first, and made (formatted)
when missing. Nothing on A is changed.
The two programs are the same code apart from addresses; their memory card code is identical and both print the same sizes
(T2MCardInitSystem: sizeof(T2MCARD_SYSBODY) = 0x22C40, sizeof(SAVEDATA) = 0x1A170), so the saves are the same format.
The launcher runs this when the game switches language (F1 -> Language).

The card format (".ps2": 16384 pages of 512 bytes + 16 spare bytes with the ECC; superblock, indirect FAT, FAT, directories
of 512-byte entries) follows mymc by Ross Ridge (public domain, ps2mc.py, ps2mc_dir.py, ps2mc_ecc.py) and the engine's own
scripts/mcformat.py; no GPL code was used. Everything is written back with fresh ECC; mcformat.py check passes on the result.
"""
import argparse, os, shutil, struct, sys, time

PAGE, SPARE, RAW = 512, 16, 528
PAGES = 16384
CLUSTER = 1024
CARD_BYTES = PAGES * RAW
SB_FMT = "<28s12sHHHHLLLLLL8x128s128sbbxx"
DIRENT_FMT = "<HHL8sLL8sL28x448s"
DF_EXISTS, DF_DIR = 0x8000, 0x0020
FREE, END, USED = 0x7FFFFFFF, 0xFFFFFFFF, 0x80000000

_PAR = [bin(b).count("1") & 1 for b in range(256)]
_COL = [sum(_PAR[b & m] << i for i, m in enumerate((0x55, 0x33, 0x0F, 0x00, 0xAA, 0xCC, 0xF0))) for b in range(256)]


def ecc_chunk(chunk):
    """3 ECC bytes of 128 data bytes ([mymc] ps2mc_ecc.py _ecc_calculate)"""
    col, l0, l1 = 0x77, 0x7F, 0x7F
    for i, b in enumerate(chunk):
        col ^= _COL[b]
        if _PAR[b]:
            l0 ^= ~i
            l1 ^= i
    return bytes((col, l0 & 0x7F, l1 & 0x7F))


def ecc_page(data):
    return b"".join(ecc_chunk(data[i:i + 128]) for i in range(0, PAGE, 128)) + bytes(SPARE - 12)


class Entry:
    def __init__(self, raw):
        (self.mode, self.unused, self.length, self.created, self.cluster, self.parent, self.modified, self.attr,
         name) = struct.unpack(DIRENT_FMT, raw)
        self.name = name.split(b"\0", 1)[0]
        self.tail = name

    def pack(self):
        name = self.name + b"\0" * (32 - len(self.name)) + self.tail[32:] if len(self.name) < 32 else self.name[:32] + self.tail[32:]
        return struct.pack(DIRENT_FMT, self.mode, self.unused, self.length, self.created, self.cluster, self.parent,
                           self.modified, self.attr, name)

    @property
    def exists(self):
        return bool(self.mode & DF_EXISTS)

    @property
    def is_dir(self):
        return bool(self.mode & DF_DIR)


class Card:
    def __init__(self, path):
        self.path = path
        self.raw = bytearray(open(path, "rb").read())
        if len(self.raw) != CARD_BYTES:
            raise SystemExit(f"{path}: {len(self.raw)} bytes, a .ps2 card image has {CARD_BYTES}")
        sb = struct.unpack(SB_FMT, self.page(0)[:struct.calcsize(SB_FMT)])
        if not sb[0].startswith(b"Sony PS2 Memory Card Format"):
            raise SystemExit(f"{path}: not formatted (the game formats a card the first time it saves)")
        (_, _, self.page_len, self.ppc, self.ppb, _, self.clusters, self.alloc_offset, self.alloc_end, self.root,
         _, _, ifc, _, _, _) = sb
        if self.page_len != PAGE or self.ppc * PAGE != CLUSTER:
            raise SystemExit(f"{path}: unusual card geometry")
        self.ifc = struct.unpack("<32L", ifc)
        per = CLUSTER // 4
        fat_clusters = -(-self.alloc_end // per)
        self.fat_where = []
        for n in range(-(-fat_clusters // per)):
            self.fat_where += struct.unpack(f"<{per}L", self.cluster(self.ifc[n]))
        self.fat_where = list(self.fat_where[:fat_clusters])
        self.fat = []
        for fc in self.fat_where:
            self.fat += struct.unpack(f"<{per}L", self.cluster(fc))
        self.fat_dirty = set()

    # raw access ----------------------------------------------------------------------------------------------------
    def page(self, n):
        return bytes(self.raw[n * RAW:n * RAW + PAGE])

    def set_page(self, n, data):
        assert len(data) == PAGE
        self.raw[n * RAW:(n + 1) * RAW] = data + ecc_page(data)

    def cluster(self, c):
        return b"".join(self.page(c * self.ppc + i) for i in range(self.ppc))

    def set_cluster(self, c, data):
        data = data + bytes(CLUSTER - len(data))
        for i in range(self.ppc):
            self.set_page(c * self.ppc + i, data[i * PAGE:(i + 1) * PAGE])

    def acluster(self, n):                # an allocatable cluster (numbered from alloc_offset)
        return self.cluster(self.alloc_offset + n)

    def set_acluster(self, n, data):
        self.set_cluster(self.alloc_offset + n, data)

    # FAT -----------------------------------------------------------------------------------------------------------
    def set_fat(self, n, v):
        self.fat[n] = v
        self.fat_dirty.add(n // (CLUSTER // 4))

    def chain(self, start):
        out, cur, seen = [], start, set()
        while True:
            if cur in seen or not 0 <= cur < self.alloc_end:
                raise SystemExit(f"{self.path}: broken cluster chain at {cur}")
            seen.add(cur)
            out.append(cur)
            v = self.fat[cur]
            if v == END:
                return out
            if not v & USED:
                raise SystemExit(f"{self.path}: a chain runs into free cluster {cur}")
            cur = v & 0x7FFFFFFF

    def alloc(self, count):
        free = [i for i in range(self.alloc_end) if not self.fat[i] & USED][:count]
        if len(free) < count:
            raise SystemExit(f"{self.path}: card full ({count} clusters needed)")
        for a, b in zip(free, free[1:]):
            self.set_fat(a, USED | b)
        if free:
            self.set_fat(free[-1], END)
        return free

    def release(self, start):
        for c in self.chain(start):
            self.set_fat(c, FREE)

    # directories ---------------------------------------------------------------------------------------------------
    def read_dir(self, start, count=None):
        data = b"".join(self.acluster(c) for c in self.chain(start))
        first = Entry(data[:512])
        n = count if count is not None else first.length
        return [Entry(data[i * 512:(i + 1) * 512]) for i in range(min(n, len(data) // 512))]

    def write_dir(self, start, entries):
        """writes the entries over the directory's chain, growing it when needed; returns the start cluster"""
        need = -(-len(entries) // 2)
        chain = self.chain(start)
        if len(chain) < need:
            more = self.alloc(need - len(chain))
            self.set_fat(chain[-1], USED | more[0])
            chain += more
        data = b"".join(e.pack() for e in entries)
        for i, c in enumerate(chain[:need]):
            self.set_acluster(c, data[i * CLUSTER:(i + 1) * CLUSTER])
        return start

    def read_file(self, e):
        data = b"".join(self.acluster(c) for c in self.chain(e.cluster)) if e.length else b""
        return data[:e.length]

    def write_file(self, data):
        if not data:
            return END & 0x7FFFFFFF if False else 0xFFFFFFFF
        chain = self.alloc(-(-len(data) // CLUSTER))
        for i, c in enumerate(chain):
            self.set_acluster(c, data[i * CLUSTER:(i + 1) * CLUSTER])
        return chain[0]

    def root_entries(self):
        return self.read_dir(self.root)

    def save(self, path=None):
        per = CLUSTER // 4
        for n in sorted(self.fat_dirty):
            self.set_cluster(self.fat_where[n], struct.pack(f"<{per}L", *self.fat[n * per:(n + 1) * per]))
        self.fat_dirty.clear()
        tmp = (path or self.path) + ".tmp"
        open(tmp, "wb").write(self.raw)
        os.replace(tmp, path or self.path)

    # saves -----------------------------------------------------------------------------------------------------------
    def folders(self, code):
        """[(index in the root, entry)] of the folders whose name starts with code"""
        return [(i, e) for i, e in enumerate(self.root_entries())
                if e.exists and e.is_dir and e.name.decode("latin-1").startswith(code)]

    def delete_folder(self, index):
        root = self.root_entries()
        e = root[index]
        for f in self.read_dir(e.cluster, e.length)[2:]:
            if f.exists and f.length and not f.is_dir:
                self.release(f.cluster)
        self.release(e.cluster)
        e.mode &= ~DF_EXISTS
        self.write_dir(self.root, root)

    def add_folder(self, folder, files, now_entry_template):
        """folder: the source folder Entry (renamed); files: [(Entry, data)] in order, "." and ".." included"""
        root = self.root_entries()
        slot = next((i for i, e in enumerate(root) if i >= 2 and not e.exists), None)
        if slot is None:
            slot = len(root)
            root.append(Entry(bytes(512)))
            root[0].length = len(root)
        out = []
        for e, data in files:
            n = Entry(e.pack())
            if e.name == b".":
                n.cluster, n.parent = 0, slot     # the parent directory (the root, cluster 0) and this folder's place in it
            elif e.name == b"..":
                pass
            elif e.is_dir:
                raise SystemExit("folders inside a save folder are not expected")
            else:
                n.cluster = self.write_file(data) if e.length else e.cluster
            out.append(n)
        first = self.alloc(1)[0]
        self.write_dir(first, out)
        f = Entry(folder.pack())
        f.cluster, f.parent, f.length = first, 0, len(out)
        root[slot] = f
        self.write_dir(self.root, root)
        return slot


def rename(name, a, b):
    s = name.decode("latin-1")
    return (b + s[len(a):]).encode("latin-1") if s.startswith(a) else name


def mirror(src_path, dst_path, a, b):
    src = Card(src_path)
    if not os.path.exists(dst_path):
        make_blank(dst_path)
    else:
        shutil.copyfile(dst_path, dst_path + ".bak")
    dst = Card(dst_path)
    # the other program's folders that the carried ones replace (same slot); its other slots stay
    incoming = {rename(e.name, a, b) for _, e in src.folders(a)}
    for i, e in sorted(dst.folders(b), key=lambda x: -x[0]):
        if e.name in incoming:
            dst.delete_folder(i)
    copied = []
    for _, folder in src.folders(a):
        entries = src.read_dir(folder.cluster, folder.length)
        files = []
        for e in entries:
            data = src.read_file(e) if e.exists and not e.is_dir and e.name not in (b".", b"..") else b""
            if e.name not in (b".", b".."):
                e.name = rename(e.name, a, b)
            files.append((e, data))
        folder.name = rename(folder.name, a, b)
        dst.add_folder(folder, files, None)
        copied.append(folder.name.decode("latin-1"))
    dst.save()
    return copied


def make_blank(path):
    here = os.path.dirname(os.path.abspath(__file__))
    for scripts in (os.path.join(here, "..", "engine", "ps2-native-recomp", "scripts"), os.environ.get("PS2_SCRIPTS", "")):
        if scripts and os.path.exists(os.path.join(scripts, "mcformat.py")):
            sys.path.insert(0, scripts)
            import mcformat
            open(path, "wb").write(mcformat.make_card(time.gmtime(time.time() + 9 * 3600)))
            return
    raise SystemExit(f"{path} does not exist and mcformat.py (the engine's scripts) was not found to make it")


def listing(path):
    c = Card(path)
    for i, e in enumerate(c.root_entries()):
        if not e.exists or e.name in (b".", b".."):
            continue
        print(f"{e.name.decode('latin-1')}/")
        if e.is_dir:
            for f in c.read_dir(e.cluster, e.length)[2:]:
                if f.exists:
                    print(f"    {f.name.decode('latin-1'):32} {f.length:8}")


def selftest():
    """A card with two made-up save folders, mirrored to a new card under the other name and back; mcformat's check must pass
    and every file must come back byte for byte."""
    import random, tempfile
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, os.path.join(here, "..", "engine", "ps2-native-recomp", "scripts"))
    import mcformat
    for _ in range(20):
        d = bytes(random.randrange(256) for _ in range(512))
        assert ecc_page(d) == mcformat.ecc_page(d)
    tmp = tempfile.mkdtemp()
    a, b = os.path.join(tmp, "us.ps2"), os.path.join(tmp, "jp.ps2")
    make_blank(a)
    card = Card(a)
    now = mcformat.tod(time.gmtime())
    want = {}
    for folder, names in (("BASLUS-21501", ["icon.sys", "icon.ico", "BASLUS-21501"]),
                          ("BASLUS-21501000", ["icon.sys", "icon.ico", "BASLUS-21501000"])):
        files = [(Entry(mcformat.dirent(0x8427, 0, 0, 0, now, b".")), b""), (Entry(mcformat.dirent(0xA426, 0, 0, 0, now, b"..")), b"")]
        for name in names:
            data = bytes(random.randrange(256) for _ in range(random.choice((964, 3000, 0x1A170))))
            files.append((Entry(mcformat.dirent(0x8497, len(data), 0, 0, now, name.encode())), data))
            want[(folder, name)] = data
        card.add_folder(Entry(mcformat.dirent(0x8427, len(files), 0, 0, now, folder.encode())), files, None)
    card.save()
    for path in (a,):
        assert subprocess_check(mcformat, path), "check failed on the made card"
    got = mirror(a, b, "BASLUS-21501", "BISLPS-25606")
    assert got == ["BISLPS-25606", "BISLPS-25606000"], got
    assert subprocess_check(mcformat, b), "check failed on the mirrored card"
    back = os.path.join(tmp, "back.ps2")
    mirror(b, back, "BISLPS-25606", "BASLUS-21501")
    c = Card(back)
    for i, e in c.folders("BASLUS-21501"):
        for f in c.read_dir(e.cluster, e.length)[2:]:
            assert c.read_file(f) == want[(e.name.decode(), f.name.decode())], f.name
    # mirroring again over an existing card replaces the folders instead of adding more
    mirror(a, b, "BASLUS-21501", "BISLPS-25606")
    assert len(Card(b).folders("BISLPS-25606")) == 2 and subprocess_check(mcformat, b)
    # a slot only the target has stays
    extra = Card(b)
    files = [(Entry(mcformat.dirent(0x8427, 0, 0, 0, now, b".")), b""), (Entry(mcformat.dirent(0xA426, 0, 0, 0, now, b"..")), b""),
             (Entry(mcformat.dirent(0x8497, 5000, 0, 0, now, b"BISLPS-25606007")), bytes(5000))]
    extra.add_folder(Entry(mcformat.dirent(0x8427, 3, 0, 0, now, b"BISLPS-25606007")), files, None)
    extra.save()
    mirror(a, b, "BASLUS-21501", "BISLPS-25606")
    names = sorted(e.name.decode() for _, e in Card(b).folders("BISLPS-25606"))
    assert names == ["BISLPS-25606", "BISLPS-25606000", "BISLPS-25606007"], names
    assert subprocess_check(mcformat, b) and os.path.exists(b + ".bak")
    print("rd_card selftest: ok (ECC, folders, files, round trip, mirror twice, mcformat check)")
    return 0


def subprocess_check(mcformat, path):
    import contextlib, io
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = mcformat.check_card(path)
    ok = rc in (0, None) and "PROBLEM" not in out.getvalue()
    if not ok:
        print(out.getvalue())
    return ok


# saves as folders (launcher v0.7.8) ------------------------------------------------------------------------------------------
import json


def stamp(raw8):
    """the card's 8-byte time (unused, s, min, h, day, month, year) as a sortable tuple"""
    _, sec, mi, h, d, mo, y = struct.unpack("<BBBBBBH", raw8)
    return (y, mo, d, h, mi, sec)


def folder_files(card, folder):
    """[(Entry, data)] of a save folder, "." and ".." included"""
    out = []
    for e in card.read_dir(folder.cluster, folder.length):
        data = card.read_file(e) if e.exists and not e.is_dir and e.name not in (b".", b"..") else b""
        out.append((e, data))
    return out


def write_folder(saves, folder, files):
    """the save folder written whole beside the old one, then put in its place; True when it changed. A folder there with a
    later date is kept (two cards of the same program, the US build and the Enhanced Edition, write the same folders)"""
    name = folder.name.decode("latin-1")
    meta = {"folder": folder.pack().hex(), "entries": [e.pack().hex() for e, _ in files]}
    target = os.path.join(saves, name)
    old_meta = os.path.join(target, "_card.json")
    if os.path.exists(old_meta):
        try:
            if stamp(Entry(bytes.fromhex(json.load(open(old_meta))["folder"])).modified) > stamp(folder.modified):
                return False
            same = json.load(open(old_meta)) == meta and all(
                e.name in (b".", b"..") or not e.exists or e.is_dir or
                open(os.path.join(target, e.name.decode("latin-1")), "rb").read() == data for e, data in files)
            if same:
                return False
        except (OSError, ValueError):
            pass
    new = target + ".new"
    shutil.rmtree(new, ignore_errors=True)
    os.makedirs(new)
    for e, data in files:
        if e.exists and not e.is_dir and e.name not in (b".", b".."):
            with open(os.path.join(new, e.name.decode("latin-1")), "wb") as f:
                f.write(data)
    with open(os.path.join(new, "_card.json"), "w") as f:
        json.dump(meta, f, indent=1)
    old = target + ".old"
    shutil.rmtree(old, ignore_errors=True)
    if os.path.exists(target):
        os.replace(target, old)
    os.replace(new, target)
    shutil.rmtree(old, ignore_errors=True)
    return True


def read_folder(path):
    """(folder Entry, [(Entry, data)]) of a save folder written by export, or None"""
    try:
        meta = json.load(open(os.path.join(path, "_card.json")))
        folder = Entry(bytes.fromhex(meta["folder"]))
        files = []
        for h in meta["entries"]:
            e = Entry(bytes.fromhex(h))
            data = b""
            if e.exists and not e.is_dir and e.name not in (b".", b".."):
                data = open(os.path.join(path, e.name.decode("latin-1")), "rb").read()
                e.length = len(data)
            files.append((e, data))
        return folder, files
    except (OSError, ValueError, KeyError, struct.error):
        return None


def export(card_path, saves, code, deletes=True):
    card = Card(card_path)
    os.makedirs(saves, exist_ok=True)
    on_card, changed = set(), []
    for _, folder in card.folders(code):
        name = folder.name.decode("latin-1")
        on_card.add(name)
        if write_folder(saves, folder, folder_files(card, folder)):
            changed.append(name)
    gone = []
    for name in (sorted(os.listdir(saves)) if deletes else []):
        if name.startswith(code) and os.path.isdir(os.path.join(saves, name)) and "." not in name and name not in on_card:
            dest = os.path.join(saves, "_deleted", name + time.strftime("-%Y%m%d-%H%M%S"))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            os.replace(os.path.join(saves, name), dest)
            gone.append(name)
    return changed, gone


def build(saves, card_path, code, other=None):
    chosen = {}   # this program's folder name -> (stamp, path, the name it has in the folder)
    names = sorted(os.listdir(saves)) if os.path.isdir(saves) else []
    for n in names:
        path = os.path.join(saves, n)
        if not os.path.isdir(path) or "." in n or n.startswith("_"):
            continue
        got = read_folder(path)
        if not got:
            continue
        when = stamp(got[0].modified)
        if n.startswith(code):
            key = n
        elif other and n.startswith(other):
            key = code + n[len(other):]
        else:
            continue
        if key not in chosen or when > chosen[key][0] or (when == chosen[key][0] and n.startswith(code)):
            chosen[key] = (when, path, n)
    tmp = card_path + ".building"
    make_blank(tmp)
    card = Card(tmp)
    for key in sorted(chosen):
        _, path, n = chosen[key]
        folder, files = read_folder(path)
        source = code if n.startswith(code) else other
        for e, _ in files:
            if e.name not in (b".", b".."):
                e.name = rename(e.name, source, code)
        folder.name = rename(folder.name, source, code)
        card.add_folder(folder, files, None)
    card.save()
    os.replace(tmp, card_path)
    return [chosen[k][2] for k in sorted(chosen)]


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list").add_argument("card")
    m = sub.add_parser("mirror")
    m.add_argument("--from", dest="src", required=True)
    m.add_argument("--to", dest="dst", required=True)
    m.add_argument("--from-code", required=True)
    m.add_argument("--to-code", required=True)
    x = sub.add_parser("export")
    x.add_argument("--card", required=True)
    x.add_argument("--to", required=True)
    x.add_argument("--code", required=True)
    x.add_argument("--keep-others", action="store_true", help="a card from before: folders not on it are not taken as deleted")
    b = sub.add_parser("build")
    b.add_argument("--from", dest="saves", required=True)
    b.add_argument("--card", required=True)
    b.add_argument("--code", required=True)
    b.add_argument("--other-code", default=None)
    sub.add_parser("selftest")
    a = ap.parse_args(argv)
    if a.cmd == "list":
        listing(a.card)
    elif a.cmd == "export":
        if not os.path.exists(a.card):
            print(f"{a.card}: no card, nothing to write out")
            return 0
        try:
            changed, gone = export(a.card, a.to, a.code, deletes=not a.keep_others)
        except SystemExit as e:
            print(f"{e}: the saves folder is left as it is")
            return 1
        print(f"{a.to}: {len(changed)} saves written" + (": " + ", ".join(changed) if changed else "") +
              (f"; {len(gone)} deleted in the game, kept in _deleted: " + ", ".join(gone) if gone else ""))
    elif a.cmd == "build":
        got = build(a.saves, a.card, a.code, a.other_code)
        print(f"{a.card}: made from {len(got)} save folders" + (": " + ", ".join(got) if got else ""))
    elif a.cmd == "selftest":
        return selftest()
    else:
        if not os.path.exists(a.src):
            print(f"{a.src}: no card yet, nothing to carry over")
            return 0
        try:
            Card(a.src)
        except SystemExit as e:
            print(f"{e}: nothing to carry over")
            return 0
        got = mirror(a.src, a.dst, a.from_code, a.to_code)
        print(f"{a.dst}: {len(got)} save folders from {a.src}" + (": " + ", ".join(got) if got else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
