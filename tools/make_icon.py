#!/usr/bin/env python3
"""Draws Halo's app icon (res/halo.ico + res/halo.png): a glowing ring on a dark rounded tile."""
from PIL import Image, ImageDraw, ImageFilter
import os

def tile(size):
    s = size * 4  # supersample
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    r = int(s * 0.22)
    # background: deep violet-blue gradient
    bg = Image.new("RGBA", (s, s))
    bd = ImageDraw.Draw(bg)
    for y in range(s):
        t = y / s
        c = (int(26 + 10 * t), int(24 + 4 * t), int(46 + 18 * t), 255)
        bd.line([(0, y), (s, y)], fill=c)
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, s - 1, s - 1], radius=r, fill=255)
    img.paste(bg, (0, 0), mask)
    # glow
    glow = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    c, rr, w = s / 2, s * 0.28, s * 0.10
    gd.ellipse([c - rr - w, c - rr - w, c + rr + w, c + rr + w], outline=(124, 140, 255, 200), width=int(w * 2))
    glow = glow.filter(ImageFilter.GaussianBlur(s * 0.05))
    img = Image.alpha_composite(img, Image.composite(glow, Image.new("RGBA", (s, s), (0, 0, 0, 0)), mask))
    d = ImageDraw.Draw(img)
    # the ring: a 3/4 arc in accent + a faint track
    box = [c - rr, c - rr, c + rr, c + rr]
    d.ellipse(box, outline=(255, 255, 255, 46), width=int(w))
    d.arc(box, start=-90, end=200, fill=(150, 168, 255, 255), width=int(w))
    # rounded ends
    import math
    for ang in (-90, 200):
        a = math.radians(ang)
        x, y = c + rr * math.cos(a) - 0, c + rr * math.sin(a)
        x -= 0; y -= 0
        cr = w / 2 - 1
        # the arc's stroke is centered inside the box edge: nudge inward by w/2
        x = c + (rr - w / 2) * math.cos(a)
        y = c + (rr - w / 2) * math.sin(a)
        d.ellipse([x - cr, y - cr, x + cr, y + cr], fill=(150, 168, 255, 255))
    d.ellipse([c - s * 0.07, c - s * 0.07, c + s * 0.07, c + s * 0.07], fill=(235, 238, 255, 255))
    return img.resize((size, size), Image.LANCZOS)

here = os.path.dirname(os.path.abspath(__file__))
res = os.path.join(here, "..", "res")
big = tile(256)
big.save(os.path.join(res, "halo.png"))
big.save(os.path.join(res, "halo.ico"), sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (128, 128), (256, 256)])
print("icon written")
