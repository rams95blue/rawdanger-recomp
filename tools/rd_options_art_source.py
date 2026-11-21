#!/usr/bin/env python3
"""Makes tools/options_art.bin: the letters the PC rows of the game's Options list are written with (rd_options.py puts the
words together from them on the player's computer). Run where Pillow and the DejaVu fonts are (not needed to play).

Three faces in the look of the game's own Options words: L (labels, 16 px, a little spaced), S (long labels, 13 px) and
V (values, 15 px, wide spacing as the game's ON / OFF). Each letter is an alpha mask in a 24 px tall cell, with its advance in
1/16 px. Codes 32-126, and 1-4 for the arrows up, down, left, right."""
from PIL import Image, ImageDraw, ImageFont
import struct, sys
FONT = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'
FACES = (('L', 16, 1.2), ('S', 13, 0.8), ('V', 15, 2.6))
EXTRA = {1: '▲', 2: '▼', 3: '◀', 4: '▶'}
out = bytearray(b'RDOF' + struct.pack('<I', len(FACES)))
for name, size, track in FACES:
    f = ImageFont.truetype(FONT, size)
    asc, _ = f.getmetrics()
    codes = list(range(32, 127)) + list(EXTRA)
    out += name.encode() + struct.pack('<BH', size, len(codes))
    for c in codes:
        ch = EXTRA.get(c, chr(c))
        adv = f.getlength(ch) + track
        w = int(f.getlength(ch) + 3)
        im = Image.new('L', (max(w, 1), 24), 0)
        ImageDraw.Draw(im).text((1, (24 - asc) // 2 - 1), ch, font=f, fill=255)
        out += struct.pack('<BHB', c, int(round(adv * 16)), im.width) + im.tobytes()
open(sys.argv[1] if len(sys.argv) > 1 else 'options_art.bin', 'wb').write(out)
print(len(out), 'bytes')
