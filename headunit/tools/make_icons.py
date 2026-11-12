#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""Draws NBTApp's placeholder iDrive images into assets/. Needs Pillow.

  row_icon_63x54.png          list-row icon: white glyph + soft dark shadow, like the factory 63x54 RGBA row icons
  content_preview_549x484.png preview beside the list: grey gradient glyph on a dark soft vignette, transparent edges
Glyph: a plain "N" in a rounded frame. Replace it when the app has a purpose.
The PNGs are tracked; run this (`make icons`) only to change them. `make pkg` packs them into the image DB.
Usage: python3 tools/make_icons.py [outdir]
"""
import sys, os
from PIL import Image, ImageDraw, ImageFilter, ImageChops
from PIL.PngImagePlugin import PngInfo

NOTICE = "Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps). License: PolyForm Strict License 1.0.0"

def glyph_mask(w, h, box, stroke, ss=8):
    """An "N" inside a rounded frame, drawn supersampled into `box` (x0, y0, x1, y1)."""
    m = Image.new("L", (w * ss, h * ss), 0)
    d = ImageDraw.Draw(m)
    x0, y0, x1, y1 = [v * ss for v in box]
    bw, bh, sw = x1 - x0, y1 - y0, stroke * ss
    d.rounded_rectangle([x0, y0, x1, y1], radius=0.22 * min(bw, bh), outline=255, width=int(sw * 0.8))
    P = lambda u, v: (x0 + u * bw, y0 + v * bh)
    def seg(a, c):
        d.line([a, c], fill=255, width=int(sw))
        for p in (a, c):
            d.ellipse([p[0] - sw / 2, p[1] - sw / 2, p[0] + sw / 2, p[1] + sw / 2], fill=255)
    seg(P(0.30, 0.72), P(0.30, 0.28))
    seg(P(0.30, 0.28), P(0.70, 0.72))
    seg(P(0.70, 0.72), P(0.70, 0.28))
    return m.resize((w, h), Image.LANCZOS)

def vgrad(w, h, top, bot, y0, y1):
    g = Image.new("RGBA", (w, h))
    px = g.load()
    for y in range(h):
        t = min(1.0, max(0.0, (y - y0) / float(max(1, y1 - y0))))
        c = int(top + (bot - top) * t)
        for x in range(w):
            px[x, y] = (c, c, c, 255)
    return g

def save(img, path):
    info = PngInfo()
    info.add_text("Copyright", NOTICE)
    img.save(path, pnginfo=info)
    print("wrote", path, img.size)

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "assets")

    W, H = 549, 484
    dark = Image.new("L", (W, H), 0)
    ImageDraw.Draw(dark).rounded_rectangle([92, 88, 458, 398], radius=70, fill=255)
    prev = vgrad(W, H, 8, 28, 80, 420)
    prev.putalpha(dark.filter(ImageFilter.GaussianBlur(30)))
    fill = vgrad(W, H, 196, 104, 126, 328)
    fill.putalpha(glyph_mask(W, H, (174, 126, 376, 328), 11, ss=4))
    prev.alpha_composite(fill)
    save(prev, os.path.join(out, "content_preview_549x484.png"))

    w, h = 63, 54
    gm = glyph_mask(w, h, (10, 5, 54, 49), 4.5)
    row = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    row.putalpha(ImageChops.offset(gm, 1, 1).filter(ImageFilter.GaussianBlur(1)).point(lambda v: int(v * 0.65)))
    white = Image.new("RGBA", (w, h), (255, 255, 255, 0))
    white.putalpha(gm)
    row.alpha_composite(white)
    save(row, os.path.join(out, "row_icon_63x54.png"))

if __name__ == "__main__":
    main()
