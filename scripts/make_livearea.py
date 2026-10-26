#!/usr/bin/env python3
"""Builds the LiveArea images from the APK icon (8-bit palette PNGs, which
the Vita installer requires). Usage: scripts/make_livearea.py <app_icon.png>"""
import os
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BG = (12, 38, 120)


def save8(img, path):
    img.convert("RGB").quantize(colors=256, method=Image.Quantize.MEDIANCUT).save(path, optimize=True)


def main(icon_path):
    icon = Image.open(icon_path).convert("RGBA")
    out = os.path.join(ROOT, "sce_sys")
    os.makedirs(os.path.join(out, "livearea", "contents"), exist_ok=True)

    base = Image.new("RGBA", (128, 128), BG + (255,))
    base.alpha_composite(icon.resize((128, 128), Image.LANCZOS))
    save8(base, os.path.join(out, "icon0.png"))

    startup = Image.new("RGBA", (280, 158), BG + (255,))
    small = icon.resize((140, 140), Image.LANCZOS)
    startup.alpha_composite(small, ((280 - 140) // 2, (158 - 140) // 2))
    save8(startup, os.path.join(out, "livearea", "contents", "startup.png"))

    bg = Image.new("RGBA", (840, 500), BG + (255,))
    big = icon.resize((320, 320), Image.LANCZOS)
    bg.alpha_composite(big, ((840 - 320) // 2, (500 - 320) // 2))
    save8(bg, os.path.join(out, "livearea", "contents", "bg.png"))


if __name__ == "__main__":
    main(sys.argv[1])
