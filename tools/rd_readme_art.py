#!/usr/bin/env python3
"""rd_readme_art.py - the README's banner and the repository's social preview, drawn from scratch (no game pictures).

  python tools/rd_readme_art.py [--version 0.9.1] [--out docs/images]

A flooded city at night after the quake: a skyline with lit windows, one tower leaning, a broken overpass, its reflection in
the water, rain, and the title on the left. Writes banner.png (1280 x 420) and social-preview.png (1280 x 640, for GitHub's
Settings > Social preview). Needs Pillow and the Noto Sans CJK / DejaVu fonts (any bold font works; see FONTS)."""
import argparse
import math
import os
import random

from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONTS = {
    "heavy": ["/usr/share/fonts/opentype/noto/NotoSansCJK-Black.ttc", "C:/Windows/Fonts/arialbd.ttf"],
    "bold": ["/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf", "C:/Windows/Fonts/arialnb.ttf", "C:/Windows/Fonts/arialbd.ttf"],
    "jp": ["/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc", "/usr/share/fonts/opentype/noto/NotoSansCJK-Black.ttc", "C:/Windows/Fonts/meiryob.ttc"],
}
NAVY_TOP, NAVY_HORIZON = (8, 13, 28), (30, 48, 82)
WATER_TOP, WATER_BOTTOM = (14, 24, 44), (5, 9, 18)
WARM, COOL = (242, 193, 78), (150, 196, 255)


def font(kind, size, index=0):
    for path in FONTS[kind]:
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, size, index=index)
            except OSError:
                continue
    return ImageFont.load_default()


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))


def sky(w, horizon):
    img = Image.new("RGB", (w, horizon))
    d = ImageDraw.Draw(img)
    for y in range(horizon):
        d.line([(0, y), (w, y)], fill=lerp(NAVY_TOP, NAVY_HORIZON, (y / horizon) ** 1.6))
    return img


def glow(size, at, radius, colour, strength):
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for k in range(24, 0, -1):
        r = radius * k / 24
        a = int(strength * (1 - k / 24) ** 1.8)
        d.ellipse([at[0] - r, at[1] - r * 0.6, at[0] + r, at[1] + r * 0.6], fill=colour + (a,))
    return layer.filter(ImageFilter.GaussianBlur(radius / 6))


def skyline(w, horizon, rng, x_from):
    """The city as an RGBA layer: buildings from x_from to the right edge, base on the horizon."""
    layer = Image.new("RGBA", (w, horizon), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    x = x_from
    while x < w:
        bw = rng.randint(28, 74)
        far = rng.random() < 0.45
        bh = rng.randint(60, 150) if far else rng.randint(110, 250)
        body = (20, 32, 56) if far else (11, 18, 34)
        top = horizon - bh
        d.rectangle([x, top, x + bw, horizon], fill=body + (255,))
        if not far:
            d.line([(x, top), (x + bw, top)], fill=(70, 100, 150, 150), width=1)
        if not far and rng.random() < 0.35:   # a roof block or an antenna
            d.rectangle([x + bw * 0.3, top - 12, x + bw * 0.6, top], fill=body + (255,))
            if rng.random() < 0.5:
                d.line([(x + bw * 0.45, top - 12), (x + bw * 0.45, top - 34)], fill=body + (255,), width=2)
        # windows: most dark (the power is out), a few still lit
        for wy in range(top + 8, horizon - 6, 9):
            for wx in range(x + 5, x + bw - 6, 8):
                p = rng.random()
                if p < 0.06:
                    d.rectangle([wx, wy, wx + 3, wy + 4], fill=WARM + (235,))
                elif p < 0.09:
                    d.rectangle([wx, wy, wx + 3, wy + 4], fill=COOL + (160,))
                elif not far and p < 0.35:
                    d.rectangle([wx, wy, wx + 3, wy + 4], fill=(26, 40, 66, 255))
        x += bw + rng.randint(-6, 4)
    return layer


def leaning_tower(w, horizon, at_x, height, lean_deg):
    """A tall tower leaning after the quake, cracked, drawn upright and turned round its foot."""
    tw = 70
    tower = Image.new("RGBA", (tw + 40, height + 40), (0, 0, 0, 0))
    d = ImageDraw.Draw(tower)
    body = (18, 29, 52, 255)
    d.rectangle([20, 30, 20 + tw, height + 40], fill=body)
    d.rectangle([34, 12, 20 + tw - 14, 30], fill=body)
    d.line([(21, 30), (21, height + 40)], fill=(110, 150, 215, 200), width=2)   # the glow catching its edge
    d.line([(34, 12), (20 + tw - 14, 12)], fill=(110, 150, 215, 160), width=2)
    rng = random.Random(7)
    for wy in range(42, height + 30, 10):
        for wx in range(28, 20 + tw - 6, 9):
            if rng.random() < 0.05:
                d.rectangle([wx, wy, wx + 4, wy + 5], fill=WARM + (220,))
    # the crack: a jagged dark line across
    pts, y = [], int(height * 0.45)
    for k in range(0, tw + 1, 7):
        pts.append((20 + k, y + rng.randint(-6, 6)))
    d.line(pts, fill=(4, 7, 14, 255), width=4)
    d.polygon([(20, y - 4), (20, y + 10), (31, y + 3)], fill=(0, 0, 0, 0))   # a bite out of the edge
    turned = tower.rotate(lean_deg, resample=Image.BICUBIC, expand=True, center=(20 + tw // 2, height + 40))
    layer = Image.new("RGBA", (w, horizon), (0, 0, 0, 0))
    layer.alpha_composite(turned, (at_x - turned.width // 2, horizon - turned.height + 18))
    return layer


def overpass(w, horizon, x0, x1, y):
    """A highway on pillars, broken in the middle, one end hanging into the gap."""
    layer = Image.new("RGBA", (w, horizon), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    deck = (13, 21, 40, 255)
    gap0, gap1 = x0 + (x1 - x0) * 0.52, x0 + (x1 - x0) * 0.62
    d.rectangle([x0, y, gap0 - 40, y + 10], fill=deck)
    d.polygon([(gap0 - 40, y), (gap0, y + 34), (gap0, y + 44), (gap0 - 40, y + 10)], fill=deck)   # the hanging end
    d.rectangle([gap1, y, x1, y + 10], fill=deck)
    for px in range(int(x0 + 30), int(x1), 90):
        if gap0 - 60 < px < gap1 + 10:
            continue
        d.rectangle([px, y + 10, px + 9, horizon], fill=deck)
    for lx in range(int(x0 + 10), int(x1), 46):   # the few lamps still on
        if gap0 - 50 < lx < gap1:
            continue
        d.ellipse([lx - 2, y - 6, lx + 2, y - 2], fill=WARM + (200,))
    return layer


def reflect(city, water_h, w):
    """The city upside down in the water: darker, bluer, rippled."""
    flipped = city.transpose(Image.FLIP_TOP_BOTTOM).crop((0, 0, w, water_h))
    out = Image.new("RGBA", (w, water_h), (0, 0, 0, 0))
    for y in range(water_h):
        shift = int(math.sin(y * 0.45) * (1 + y / 25) + math.sin(y * 0.13) * 2)
        row = flipped.crop((0, y, w, y + 1))
        out.alpha_composite(row, (shift, y))
    r, g, b, a = out.split()
    fade = Image.linear_gradient("L").resize((w, water_h)).point(lambda v: 255 - v)
    a = Image.composite(a, Image.new("L", a.size, 0), fade).point(lambda v: int(v * 0.55))
    return Image.merge("RGBA", (r.point(lambda v: v * 6 // 10), g.point(lambda v: v * 7 // 10), b, a))


def rain(size, rng, count, alpha):
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for _ in range(count):
        x, y = rng.uniform(-60, size[0]), rng.uniform(-40, size[1])
        ln = rng.uniform(14, 34)
        d.line([(x, y), (x + ln * 0.28, y + ln)], fill=(200, 220, 255, rng.randint(alpha // 3, alpha)), width=1)
    return layer


def hazard(w, h):
    stripe = Image.new("RGBA", (w, h), (0, 0, 0, 255))
    d = ImageDraw.Draw(stripe)
    for x in range(-h, w + h, 2 * h):
        d.polygon([(x, h), (x + h, 0), (x + 2 * h, 0), (x + h, h)], fill=WARM + (255,))
    return stripe


def scene(w, h, horizon, seed=95):
    rng = random.Random(seed)
    img = sky(w, h).convert("RGBA")
    img.alpha_composite(glow((w, h), (int(w * 0.72), horizon - 60), 420, (120, 160, 230), 150))
    city = Image.new("RGBA", (w, horizon), (0, 0, 0, 0))
    city.alpha_composite(skyline(w, horizon, rng, int(w * 0.30)))
    city.alpha_composite(leaning_tower(w, horizon, int(w * 0.78), int(horizon * 0.78), -9))
    city.alpha_composite(overpass(w, horizon, int(w * 0.50), w + 20, horizon - 58))
    img.alpha_composite(city, (0, 0))
    water = Image.new("RGBA", (w, h - horizon))
    dw = ImageDraw.Draw(water)
    for y in range(h - horizon):
        dw.line([(0, y), (w, y)], fill=lerp(WATER_TOP, WATER_BOTTOM, y / max(1, h - horizon - 1)) + (255,))
    img.alpha_composite(water, (0, horizon))
    img.alpha_composite(reflect(city, h - horizon, w), (0, horizon))
    lines = Image.new("RGBA", (w, h - horizon), (0, 0, 0, 0))   # light on the water's ripples
    dl = ImageDraw.Draw(lines)
    for _ in range(140):
        x, y = rng.uniform(w * 0.3, w), rng.uniform(2, h - horizon)
        dl.line([(x, y), (x + rng.uniform(10, 40), y)], fill=(150, 180, 230, rng.randint(20, 60)), width=1)
    img.alpha_composite(lines, (0, horizon))
    # the left side darker, for the words
    shade = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ds = ImageDraw.Draw(shade)
    for x in range(int(w * 0.62)):
        t = 1 - x / (w * 0.62)
        ds.line([(x, 0), (x, h)], fill=(5, 9, 20, int(235 * t ** 1.3)))
    img.alpha_composite(shade)
    img.alpha_composite(rain((w, h), rng, int(w * h / 900), 70))
    img.alpha_composite(rain((w, h), rng, int(w * h / 4000), 140).filter(ImageFilter.GaussianBlur(0.6)))
    return img


def words(img, x, y, version, big, tagline=None):
    over = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(over)
    kicker = font("bold", int(big * 0.22))
    d.text((x + 2, y), "NATIVE PC PORT  ·  STATIC RECOMPILATION", font=kicker, fill=COOL + (255,))
    y += int(big * 0.36)
    title = font("heavy", big)
    tx, ty = x, y - int(big * 0.18)
    d.text((tx, ty), "RAW DANGER!", font=title, fill=(246, 248, 252, 255))
    tw = d.textlength("RAW DANGER!", font=title)
    y += int(big * 1.10)
    d.rectangle([x + 2, y, x + 2 + int(tw * 0.42), y + max(4, big // 22)], fill=WARM + (255,))
    y += int(big * 0.16)
    sub = font("jp", int(big * 0.27))
    d.text((x + 2, y), "絶体絶命都市2  ·  ENHANCED EDITION", font=sub, fill=(214, 224, 240, 255))
    y += int(big * 0.46)
    pill_f = font("bold", int(big * 0.19))
    label = f"rawdanger-recomp  v{version}"
    pw = d.textlength(label, font=pill_f) + 26
    ph = int(big * 0.33)
    d.rounded_rectangle([x + 2, y, x + 2 + pw, y + ph], radius=ph // 2, fill=(255, 255, 255, 22), outline=(255, 255, 255, 90))
    d.text((x + 15, y + ph * 0.16), label, font=pill_f, fill=(236, 240, 248, 255))
    if tagline:
        y += ph + int(big * 0.30)
        tf = font("bold", int(big * 0.21))
        for line in tagline:
            d.text((x + 2, y), line, font=tf, fill=(180, 194, 216, 255))
            y += int(big * 0.31)
    shadow = over.split()[3].filter(ImageFilter.GaussianBlur(big / 18))
    img.alpha_composite(Image.merge("RGBA", (Image.new("L", img.size, 0),) * 3 + (shadow.point(lambda v: v * 3 // 4),)))
    img.alpha_composite(over)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--version", default="0.9.1")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "images"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    banner = scene(1280, 420, 300)
    words(banner, 64, 92, a.version, 104)
    banner.alpha_composite(hazard(1280, 8), (0, 412))
    banner.convert("RGB").save(os.path.join(a.out, "banner.png"), optimize=True)
    social = scene(1280, 640, 448, seed=96)
    words(social, 72, 132, a.version, 124, tagline=["Raw Danger! (PS2) running natively on Windows, built from your own discs:",
                                                   "widescreen, high resolution, Japanese voices, orbit camera, achievements."])
    social.alpha_composite(hazard(1280, 10), (0, 630))
    social.convert("RGB").save(os.path.join(a.out, "social-preview.png"), optimize=True)
    print("wrote", os.path.join(a.out, "banner.png"), "and social-preview.png")


if __name__ == "__main__":
    main()
