#!/usr/bin/env python3
"""Draw the LiveArea "Settings" button (sce_sys/livearea/contents/settings.png).

The button sits in frame3 of the a1 style (the 840x150 strip under the gate)
and links to psgm:play?titleid=SRRV00001&param=settings. Requires Pillow and
the macOS "Arial Rounded Bold" font. Usage: python3 scripts/make_settings_button.py
"""
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONT = '/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf'
W, H, S = 300, 72, 4  # final size and supersampling factor


def save8(image, path):
    # Same palette conversion as make_livearea.py: an exact transparent index.
    rgba = image.convert('RGBA')
    indexed = rgba.quantize(colors=255, method=Image.Quantize.FASTOCTREE)
    palette = indexed.getpalette('RGBA')
    palette += [0] * (1024 - len(palette))
    palette[1020:1024] = [0, 0, 0, 0]
    indexed.putpalette(palette, rawmode='RGBA')
    indexed.paste(255, (0, 0), rgba.getchannel('A').point(lambda alpha: 255 if alpha == 0 else 0))
    indexed.save(path, optimize=True)


def gear(draw, cx, cy, r, color):
    teeth, outer, inner = 8, r, r * 0.72
    points = []
    for i in range(teeth * 4):
        angle = math.pi * 2 * i / (teeth * 4)
        radius = outer if i % 4 in (1, 2) else inner
        points.append((cx + radius * math.cos(angle), cy + radius * math.sin(angle)))
    draw.polygon(points, fill=color)
    hole = r * 0.32
    draw.ellipse((cx - hole, cy - hole, cx + hole, cy + hole), fill=(0, 0, 0, 0))


def main():
    w, h = W * S, H * S
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    shadow = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((6 * S, 8 * S, w - 2 * S, h - 1 * S), radius=30 * S, fill=(0, 0, 0, 120))
    img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(3 * S)))

    body = (3 * S, 3 * S, w - 6 * S, h - 7 * S)
    # Yellow rim, white ring, blue gradient face (the game's menu colors).
    ImageDraw.Draw(img).rounded_rectangle(body, radius=30 * S, fill=(255, 200, 30, 255))
    inset = lambda box, d: (box[0] + d, box[1] + d, box[2] - d, box[3] - d)
    ImageDraw.Draw(img).rounded_rectangle(inset(body, 4 * S), radius=26 * S, fill=(255, 255, 255, 255))
    face = inset(body, 7 * S)
    gradient = Image.new('RGBA', (w, h))
    gd = ImageDraw.Draw(gradient)
    for y in range(h):
        t = y / h
        gd.line((0, y, w, y), fill=(int(40 + 10 * t), int(110 - 60 * t), int(235 - 85 * t), 255))
    mask = Image.new('L', (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle(face, radius=23 * S, fill=255)
    img.paste(gradient, (0, 0), mask)
    gloss = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(gloss).rounded_rectangle((face[0] + 4 * S, face[1] + 2 * S, face[2] - 4 * S, (face[1] + face[3]) // 2),
                                            radius=20 * S, fill=(255, 255, 255, 55))
    img.alpha_composite(gloss)

    draw = ImageDraw.Draw(img)
    cy = (face[1] + face[3]) / 2
    gear(draw, face[0] + 34 * S, cy, 17 * S, (255, 210, 40, 255))
    font = ImageFont.truetype(FONT, 30 * S)
    text = 'SETTINGS'
    left, top, right, bottom = draw.textbbox((0, 0), text, font=font)
    tx = face[0] + 64 * S + ((face[2] - face[0] - 64 * S) - (right - left)) / 2 - left
    ty = cy - (bottom - top) / 2 - top
    draw.text((tx + 2 * S, ty + 3 * S), text, font=font, fill=(10, 30, 90, 255))
    draw.text((tx, ty), text, font=font, fill=(255, 255, 255, 255))

    out = img.resize((W, H), Image.Resampling.LANCZOS)
    target = ROOT / 'sce_sys/livearea/contents/settings.png'
    save8(out, target)
    print(target)


if __name__ == '__main__':
    main()
