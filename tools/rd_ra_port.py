#!/usr/bin/env python3
"""rd_ra_port.py - RetroAchievements' set for Raw Danger! moved to the US program.

  rd.py ra-port                       (achievements/ra/5616.json -> achievements/ra/5616.us.json)
  python tools/rd_ra_port.py [set.json] [out.json]

RetroAchievements' set 5616 is made for the European and the Japanese releases: each memory read starts with
"I:0xH348faa*N", the byte at 0x348FAA (0 in the European program, 1 in the Japanese one) times a negative number, which
moves the address from the European layout to the Japanese one. The US program has 13 there and its data elsewhere, so the
set reads the wrong places on it. This writes a copy for the US program: every such read is taken to the Japanese address,
then through achievements/ra_us_addresses.txt (the Japanese program's data addresses and the same data's US addresses, found
by matching the two programs' code) to the US address, and the version step is dropped. Reads that follow another add-address
step (offsets inside a structure the game points to) stay as they are. Checked on the opening narration: the set's map number
reads 01000000 ("Geo City is the safest city ever", the narration), the scene 21 and the difficulty Normal.

Offline, no account; the original file is not changed.
"""
import bisect, json, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION = re.compile(r'^I:0xH348faa\*(\d+)$', re.I)
OPERAND = re.compile(r'(?<![0-9A-Za-z])([dpb~]?)(0x[A-Z ]?|f[FB])([0-9a-fA-F]+)')


class Map:
    def __init__(self, path):
        self.jp, self.us = [], []
        for line in open(path, encoding='utf-8'):
            line = line.split('#', 1)[0].split()
            if len(line) == 2:
                self.jp.append(int(line[0], 16)); self.us.append(int(line[1], 16))

    def __call__(self, a, window=0x4000):
        k = bisect.bisect_right(self.jp, a) - 1
        if k < 0 or a - self.jp[k] > window:
            raise KeyError(f'no US address for 0x{a:08X}')
        return a - self.jp[k] + self.us[k]


def move(cond, shift, xlate):
    """the memory operands of one condition, moved: shift (the Japanese step) then the US map"""
    flag = re.match(r'^[A-Z]:', cond)
    head = flag.group(0) if flag else ''
    body = cond[len(head):]

    def one(m):
        pre, size, addr = m.group(1), m.group(2), int(m.group(3), 16)
        return f"{pre}{size}{xlate((addr + shift) & 0xffffffff):x}"
    return head + OPERAND.sub(one, body)


def port_trigger(mem, xlate):
    groups = re.split(r'(?<!0x)S', mem)
    out = []
    for group in groups:
        conds = group.split('_') if group else []
        new, shift, relative = [], None, False
        for c in conds:
            m = VERSION.match(c)
            if m:
                n = int(m.group(1)); shift = n - (1 << 32) if n >= 1 << 31 else n
                continue
            if shift is not None:
                new.append(move(c, shift, xlate))
            elif relative:
                new.append(c)          # an offset inside what the previous add-address step points to
            else:
                new.append(move(c, 0, xlate))
            relative = c.startswith('I:')
            shift = None
        out.append('_'.join(new))
    return 'S'.join(out)


def port(data, xlate):
    patch = data.get('PatchData', data)
    bad = []
    for a in patch.get('Achievements', []):
        try:
            a['MemAddr'] = port_trigger(a['MemAddr'], xlate)
        except KeyError as e:
            bad.append(f"{a['Title']}: {e}")
            a['MemAddr'] = '0=1'       # never: a read we cannot place stays off instead of firing at random
    patch['Leaderboards'] = []          # leaderboards are not kept offline
    patch['RichPresencePatch'] = ''     # display text for an online profile; not used offline
    patch['Title'] = patch.get('Title', '') + ' [US program, rawdanger-recomp]'
    return bad


def main(argv):
    src = argv[0] if argv else os.path.join(ROOT, 'achievements', 'ra', '5616.json')
    dst = argv[1] if len(argv) > 1 else src[:-5] + '.us.json'
    xlate = Map(os.path.join(ROOT, 'achievements', 'ra_us_addresses.txt'))
    data = json.load(open(src, encoding='utf-8'))
    bad = port(data, xlate)
    json.dump(data, open(dst, 'w', encoding='utf-8'), ensure_ascii=False, separators=(',', ':'))
    n = len(data.get('PatchData', data).get('Achievements', []))
    print(f"{dst}: {n} achievements for the US program" + (f", {len(bad)} left off:" if bad else ""))
    for b in bad:
        print('  ' + b)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
