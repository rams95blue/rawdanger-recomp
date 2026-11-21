#!/usr/bin/env python3
"""rd_ra_logic.py - RetroAchievements' set for Raw Danger! (achievements/ra/5616.json) as plain text.

  rd.py ra-logic            (or: python tools/rd_ra_logic.py [set.json] [out.txt])

Writes achievements/ra/5616_logic.txt: each achievement's title, points, what to do (its description) and its logic,
condition by condition, in words. Read only, offline. "Add address" lines are pointer steps: the value read there (times the
number given, often a negative number written the unsigned way) is added to the address of the next condition.
"""
import json, os, re, sys

SIZES = {'': '16-bit', 'H': '8-bit', 'X': '32-bit', 'W': '24-bit', 'L': 'low 4 bits of', 'U': 'high 4 bits of', 'M': 'bit 0 of',
         'N': 'bit 1 of', 'O': 'bit 2 of', 'P': 'bit 3 of', 'Q': 'bit 4 of', 'R': 'bit 5 of', 'S': 'bit 6 of', 'T': 'bit 7 of',
         'K': 'set bits in', 'I': '16-bit BE', 'J': '24-bit BE', 'G': '32-bit BE'}
FLAGS = {'R': 'Reset if', 'P': 'Pause if', 'A': 'Add source', 'B': 'Sub source', 'C': 'Add hits', 'D': 'Sub hits', 'N': 'And next',
         'O': 'Or next', 'M': 'Measured', 'Q': 'Measured if', 'I': 'Add address', 'T': 'Trigger', 'Z': 'Reset next if',
         'K': 'Remember', 'G': 'Measured %'}
OPS = {'=': '=', '!=': '!=', '<': '<', '<=': '<=', '>': '>', '>=': '>='}


def operand(t):
    t = t.strip()
    pre = ''
    if t[:1] in 'dp~b' and len(t) > 1 and not t.startswith('0x'):
        pre = {'d': 'last frame\'s ', 'p': 'prior ', '~': 'inverted ', 'b': 'BCD '}[t[0]]
        t = t[1:]
    m = re.fullmatch(r'0x([A-Z ]?)([0-9a-fA-F]+)', t)
    if m:
        return f"{pre}{SIZES.get(m.group(1).strip(), m.group(1))} [0x{int(m.group(2), 16):08X}]"
    m = re.fullmatch(r'f([FB])([0-9a-fA-F]+)', t)
    if m:
        return f"{pre}float{'' if m.group(1) == 'F' else ' BE'} [0x{int(m.group(2), 16):08X}]"
    if re.fullmatch(r'h[0-9a-fA-F]+', t):
        return str(int(t[1:], 16))
    if re.fullmatch(r'\d+', t):
        v = int(t)
        return f"{v - 2**32}" if v >= 2**31 else str(v)
    if t == '{recall}':
        return 'remembered value'
    return t


def condition(raw):
    m = re.fullmatch(r'(?:([A-Z]):)?(.+?)(?:(\*|/|&|\^|%|\+|-)(.+?))?(?:(=|!=|<=|>=|<|>)(.+?))?(?:\.(\d+)\.)?', raw.strip())
    if not m:
        return raw
    flag, left, op, right, cmp_, value, hits = m.groups()
    text = operand(left)
    if op:
        text += f" {op} {operand(right)}"
    if cmp_:
        text += f" {OPS[cmp_]} {operand(value)}"
    if hits and hits != '0':
        text += f"  ({hits} times)"
    return (FLAGS.get(flag, flag) + ': ' if flag else '') + text


def describe(mem):
    out = []
    for gi, group in enumerate(re.split(r'(?<!0x)S', mem)):
        out.append('  core:' if gi == 0 else f'  or alternative {gi}:')
        for raw in group.split('_'):
            if raw:
                out.append('    ' + condition(raw))
    return out


def main(argv):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    src = argv[0] if argv else os.path.join(root, 'achievements', 'ra', '5616.json')
    dst = argv[1] if len(argv) > 1 else os.path.splitext(src)[0] + '_logic.txt'
    data = json.load(open(src, encoding='utf-8'))
    patch = data.get('PatchData', data)
    lines = [f"{patch.get('Title', '')} - RetroAchievements set {patch.get('ID', '')}, {len(patch['Achievements'])} achievements",
             "Kept offline in achievements\\ra\\5616.json; the game checks it on this PC. Addresses are the PS2's main memory.", '']
    for a in sorted(patch['Achievements'], key=lambda x: (x.get('Flags') != 3, x['ID'])):
        kind = 'unofficial' if a.get('Flags') == 5 else 'official'
        lines.append(f"==== {a['Title']}  ({a['Points']} points, {kind}, ID {a['ID']}{', ' + a['Type'] if a.get('Type') else ''})")
        lines.append(f"What to do: {a['Description']}")
        lines += describe(a['MemAddr'])
        lines.append('')
    for lb in patch.get('Leaderboards', []):
        lines.append(f"==== Leaderboard: {lb['Title']} - {lb.get('Description', '')} (not checked offline)")
    open(dst, 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    print(f"{dst}: {len(patch['Achievements'])} achievements")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
