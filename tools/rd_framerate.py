#!/usr/bin/env python3
"""rd_framerate.py: the frame-rate constants of Raw Danger's US program, from the European one (docs/FRAMERATE.md).

The US and Japanese programs were compiled for 20 steps a second, the European one (SLES_545.87) for 30: the same code, with
the frame rate folded into constants. This compares the two programs function by function (both have 6207, in the same order)
and lists every constant whose change is a frame-rate change (x1.5, x2/3, x4/9 and the rounded counts), in the code (lui/ori
pairs, li, slti) and in the data (floats). The vblanks a step (3 -> 2) is one of them.

    rd.py framerate --us SLUS_215.01 --eu SLES_545.87 [--fps 30|60] [--pnach out.pnach]

--fps 60 extrapolates (the same law three times instead of one and a half); not right yet (docs/FRAMERATE.md).
"""
import argparse, math, struct, sys

def segments(path):
    b = open(path, "rb").read()
    ph, n = struct.unpack_from("<I", b, 0x1C)[0], struct.unpack_from("<H", b, 0x2C)[0]
    for i in range(n):
        t, off, va, pa, fs, ms = struct.unpack_from("<6I", b, ph + 32 * i)
        if t == 1 and fs:
            d = b[off:off + fs]
            return va, list(struct.unpack("<%dI" % (len(d) // 4), d[:len(d) // 4 * 4]))
    sys.exit(f"{path}: no loadable code")

def funcs(w):
    starts = [i for i, x in enumerate(w) if (x & 0xFFFF0000) == 0x27BD0000 and (x & 0x8000)]
    return list(zip(starts, starts[1:] + [len(w)]))

def mask(w):
    op = w >> 26
    if op in (2, 3): return w & 0xFC000000
    if op in (1, 4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17): return w
    return w & 0xFFFF0000 if op >= 8 else w

fl = lambda v: struct.unpack("<f", struct.pack("<I", v & 0xFFFFFFFF))[0]
fw = lambda f: struct.unpack("<I", struct.pack("<f", f))[0]
RATIOS = [1.5, 2 / 3, 4 / 9, 0.467, 1.6, 1.333, 1.25, 1.429, 0.8, 1.167, 0.75]
# not frame rate: a sound or text number, a table's size, a place, a bit mask, a PAL screen size (looked at one by one)
EXCLUDE = {0x19C390, 0x19C398, 0x209670, 0x24B68C, 0x348728, 0x144FDC}
fpsish = lambda r: any(abs(r - x) < 0.012 for x in RATIOS)
isfloat = lambda v: 0x30000000 < (v & 0x7FFFFFFF) < 0x50000000

def scale(vu, ve, flt, fps):
    if fps == 30: return ve
    a, b = (fl(vu), fl(ve)) if flt else (vu, ve)
    if a == 0 or b == 0 or (a < 0) != (b < 0): return ve
    if flt and 0 < a < 1 and 0 < b < 1 and abs(math.log(b) / math.log(a) - 2 / 3) < 0.03:
        return fw(a ** (1 / 3))
    v = a * 3 ** (math.log(b / a) / math.log(1.5))
    return fw(v) if flt else int(round(v))

def code_words(ub, uw, eb, ew, fps):
    words, notes = {}, []
    rt, rs = (lambda w: (w >> 16) & 31), (lambda w: (w >> 21) & 31)
    for (a, b), (c, d) in zip(funcs(uw), funcs(ew)):
        if b - a != d - c: continue
        lu, le = {}, {}
        for k in range(b - a):
            x, y = uw[a + k], ew[c + k]
            ua = ub + 4 * (a + k)
            if x >> 26 != y >> 26: continue
            op = x >> 26
            if op == 0xF:
                lu[rt(x)] = (ua, x & 0xFFFF); le[rt(y)] = y & 0xFFFF
                continue
            if op in (0xD, 9) and rs(x) == rt(x) and rt(x) in lu and rs(y) == rt(y) and rt(y) in le:
                (hia, uh), eh = lu.pop(rt(x)), le.pop(rt(y))
                sx = lambda h, l: ((h << 16) + ((l ^ 0x8000) - 0x8000)) & 0xFFFFFFFF if op == 9 else (h << 16) | l
                vu, ve = sx(uh, x & 0xFFFF), sx(eh, y & 0xFFFF)
                if vu == ve or hia in EXCLUDE or not (isfloat(vu) and isfloat(ve)) or not fpsish(fl(ve) / fl(vu)): continue
                nv = scale(vu, ve, True, fps)
                nhi, nlo = (nv >> 16) & 0xFFFF, nv & 0xFFFF
                if op == 9 and nlo & 0x8000: nhi = (nhi + 1) & 0xFFFF
                words[hia] = (uw[(hia - ub) // 4] & 0xFFFF0000) | nhi
                words[ua] = (x & 0xFFFF0000) | nlo
                notes.append((hia, "float %g -> %g" % (fl(vu), fl(nv))))
                continue
            if op in (9, 0xD) and rs(x) == 0 and x != y and ua not in EXCLUDE:
                vu = (x & 0xFFFF) if op == 0xD else ((x & 0xFFFF) ^ 0x8000) - 0x8000
                ve = (y & 0xFFFF) if op == 0xD else ((y & 0xFFFF) ^ 0x8000) - 0x8000
                if vu and fpsish(ve / vu):
                    nv = scale(vu, ve, False, fps)
                    words[ua] = (x & 0xFFFF0000) | (nv & 0xFFFF); notes.append((ua, "li %d -> %d" % (vu, nv)))
                continue
            if op in (0xA, 0xB) and x != y and ua not in EXCLUDE:
                vu, ve = ((x & 0xFFFF) ^ 0x8000) - 0x8000, ((y & 0xFFFF) ^ 0x8000) - 0x8000
                if vu and fpsish(ve / vu):
                    nv = scale(vu, ve, False, fps)
                    words[ua] = (x & 0xFFFF0000) | (nv & 0xFFFF); notes.append((ua, "slti %d -> %d" % (vu, nv)))
        for r, (ua, uh) in lu.items():
            if r in le and uh != le[r] and not 0x10 <= uh <= 0x90 and ua not in EXCLUDE:
                vu, ve = uh << 16, le[r] << 16
                if fl(vu) and fpsish(fl(ve) / fl(vu)):
                    nv = scale(vu, ve, True, fps)
                    words[ua] = (uw[(ua - ub) // 4] & 0xFFFF0000) | ((nv >> 16) & 0xFFFF)
                    notes.append((ua, "float %g -> %g" % (fl(vu), fl(nv & 0xFFFF0000))))
    return words, notes

def data_words(ub, uw, eb, ew, fps, start=0x380000, W=1024):
    words, notes = {}, []
    i, prev = (start - ub) // 4, 0
    while i + W < len(uw):
        blk, best = uw[i:i + W], None
        for off in sorted(range(-0x100, 0x101), key=lambda o: abs(o - prev)):
            j = i + off
            if j < 0 or j + W > len(ew): continue
            m = sum(1 for p, q in zip(blk, ew[j:j + W]) if p == q)
            if best is None or m > best[1]: best = (off, m)
            if m > W * 0.9: break
        off, m = best; prev = off
        if m > W * 0.5:
            for k, (a, b) in enumerate(zip(blk, ew[i + off:i + off + W])):
                if a != b and isfloat(a) and isfloat(b) and fpsish(fl(b) / fl(a)):
                    ua = ub + 4 * (i + k)
                    words[ua] = scale(a, b, True, fps); notes.append((ua, "data %g -> %g" % (fl(a), fl(words[ua]))))
        i += W
    return words, notes

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--us", required=True); ap.add_argument("--eu", required=True)
    ap.add_argument("--fps", type=int, default=30, choices=(30, 60)); ap.add_argument("--pnach")
    a = ap.parse_args(argv)
    ub, uw = segments(a.us); eb, ew = segments(a.eu)
    cw, cn = code_words(ub, uw, eb, ew, a.fps)
    dw, dn = data_words(ub, uw, eb, ew, a.fps)
    words = {**cw, **dw}
    print(f"{len(cw)} code words, {len(dw)} data words for {a.fps} steps a second", file=sys.stderr)
    if a.pnach:
        notes = dict(cn + dn)
        with open(a.pnach, "w", newline="\r\n") as f:
            for ad in sorted(words):
                f.write("patch=1,EE,%08X,word,%08X%s\n" % (ad, words[ad], ("  // " + notes[ad]) if ad in notes else ""))
    return 0

if __name__ == "__main__":
    sys.exit(main())
