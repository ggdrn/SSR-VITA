#!/usr/bin/env python3
"""Extract the original APK's title sprites and build Vita LiveArea images.

Requires Pillow and UnityPy. Usage: python3 scripts/make_livearea.py ../srr224.apk
Sources/crop provenance are retained under assets/livearea/.
"""
import argparse
import hashlib
import json
import struct
import zipfile
from pathlib import Path

import UnityPy
from PIL import Image, ImageOps

ROOT = Path(__file__).resolve().parents[1]


def save8(image, path):
    # Palette PNGs with a transparent index where needed; supported by Vita.
    rgba = image.convert('RGBA')
    indexed = rgba.quantize(colors=255, method=Image.Quantize.FASTOCTREE)
    # Octree averaging can turn alpha=0 into alpha=1 when it groups edge
    # colors. Reserve one exact transparent entry so the circular icon and
    # logo never acquire a faint rectangular background on the Vita.
    palette = indexed.getpalette('RGBA')
    palette += [0] * (1024 - len(palette))
    palette[1020:1024] = [0, 0, 0, 0]
    indexed.putpalette(palette, rawmode='RGBA')
    indexed.paste(255, (0, 0), rgba.getchannel('A').point(lambda alpha: 255 if alpha == 0 else 0))
    indexed.save(path, optimize=True)


def letterbox(image, size):
    canvas = Image.new('RGBA', size, (0, 0, 0, 255))
    fitted = ImageOps.contain(image, size, Image.Resampling.LANCZOS)
    canvas.alpha_composite(fitted, ((size[0] - fitted.width) // 2, (size[1] - fitted.height) // 2))
    return canvas


def build_title_logo(scene, crops, padding, names):
    """Reproduce static title-logo layers from the APK's actual NGUI scene.

    Widget size, padding, tint, depth, local position and scale all matter:
    using just the word crops loses the white relief, stars and blue base.
    """
    objects = {o.path_id: o for o in scene.objects}
    selected = []
    sprite_keys = {name: key for key, name in names.items()}
    for obj in scene.objects:
        if obj.type.name != 'MonoBehaviour':
            continue
        raw = obj.get_raw_data()
        key = next((key for name, key in sprite_keys.items()
                    if struct.pack('<I', len(name)) + name.encode() in raw), None)
        if key is None or len(raw) < 56:
            continue
        widget = obj.read(check_read=False)
        go = widget.m_GameObject.read()
        transform = None
        for component in go.m_Component:
            pointer = component[1] if isinstance(component, tuple) else component.component
            candidate = objects.get(pointer.m_PathID)
            if candidate and candidate.type.name == 'Transform':
                transform = candidate.read_typetree()
        # The second logo hierarchy is a duplicate; use the original one.
        if not transform or transform['m_Father']['m_PathID'] != 337:
            continue
        color = struct.unpack_from('<4f', raw, 24)
        pivot, width, height, depth = struct.unpack_from('<4i', raw, 40)
        if not go.m_IsActive or not raw[8] or color[3] <= 0:
            continue
        assert pivot == 4 # centered NGUI widget
        selected.append(dict(sprite=names[key], key=key, width=width, height=height,
                             depth=depth, color=list(color),
                             position=transform['m_LocalPosition'], scale=transform['m_LocalScale']))
    assert len(selected) == 6, 'Expected white base, star, blue base and three words'
    canvas = Image.new('RGBA', (1280, 960))
    for layer in sorted(selected, key=lambda layer: layer['depth']):
        key = layer['key']
        left, right, top, bottom = padding[key]
        crop = crops[key]
        sprite = Image.new('RGBA', (crop.width + left + right, crop.height + top + bottom))
        sprite.alpha_composite(crop, (left, top))
        scale, position = layer['scale'], layer['position']
        size = (round(layer['width'] * scale['x']), round(layer['height'] * scale['y']))
        sprite = sprite.resize(size, Image.Resampling.LANCZOS)
        color = layer['color']
        sprite = Image.merge('RGBA', tuple(channel.point(lambda v, factor=factor: round(v * factor))
                             for channel, factor in zip(sprite.split(), color)))
        at = (round(640 + position['x'] - sprite.width / 2),
              round(480 - position['y'] - sprite.height / 2))
        canvas.alpha_composite(sprite, at)
    return canvas.crop(canvas.getbbox()), selected


def build_game_splash(env, archive, chunks, image):
    # Keep the APK texture's dimensions/format/serialized lengths intact.
    # The UI rectangle supplies the final 16:9 aspect; only RGB mip bytes
    # are overlaid at read time. The APK is never rewritten on storage.
    obj = next(o for o in env.objects if o.type.name == 'Texture2D' and o.read().m_Name == 'Splash3')
    raw = obj.get_raw_data()
    assert raw[:12] == bytes.fromhex('0700000053706c6173683300')
    assert struct.unpack_from('<4I', raw, 12) == (2048, 512, 4194309, 3)
    body = 68
    assert struct.unpack_from('<I', raw, body - 4)[0] == 4194309
    w, h = 2048, 512
    levels = []
    while True:
        levels.append(image.convert('RGB').resize((w, h), Image.Resampling.LANCZOS)
                      .transpose(Image.Transpose.FLIP_TOP_BOTTOM).tobytes())
        if w == 1 and h == 1:
            break
        w, h = max(1, w // 2), max(1, h // 2)
    payload = b''.join(levels)
    assert len(payload) == 4194309
    (ROOT / 'assets/game-splash.rgb').write_bytes(payload)
    checksum = 2166136261
    for byte in payload:
        checksum = ((checksum ^ byte) * 16777619) & 0xffffffff
    start, end = obj.byte_start + body, obj.byte_start + body + len(payload)
    selected, base = [], 0
    for name in chunks:
        length = archive.getinfo(name).file_size
        if base < end and base + length > obj.byte_start:
            selected.append((name, base, length))
        base += length
    header = ('// Generated Splash3 RGB24 mip overlay; original serialized header and chunk lengths guarded.\n'
              '#pragma once\n#include <stdint.h>\n'
              f'#define GAME_SPLASH_START {start}u\n#define GAME_SPLASH_SIZE {len(payload)}u\n'
              f'#define GAME_SPLASH_HASH 0x{checksum:08x}u\n#define GAME_SPLASH_HEADER_AT {obj.byte_start}u\n'
              'static const uint8_t game_splash_header[] = {' + ','.join(f'0x{x:02x}' for x in raw[:body]) + '};\n'
              'static const struct { const char *name; uint32_t base, size; } game_splash_parts[] = {\n' +
              ''.join(f'  {{"{name}", {base}u, {length}u}},\n' for name, base, length in selected) + '};\n')
    (ROOT / 'src/util/splash_art_data.h').write_text(header)
    return {'texture': 'Splash3', 'format': 'RGB24', 'payload_bytes': len(payload),
            'payload_fnv1a': f'{checksum:08x}', 'fan_image_sha256': hashlib.sha256(image.tobytes()).hexdigest()}


def main(apk, background=None):
    source = ROOT / 'assets/livearea'
    target = ROOT / 'sce_sys/livearea/contents'
    source.mkdir(parents=True, exist_ok=True)
    target.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(apk) as z:
        icon_entry = 'res/mipmap-xxxhdpi-v4/ic_launcher_round.png'
        icon_bytes = z.read(icon_entry)
        (source / 'apk-icon.png').write_bytes(icon_bytes)
        entry = 'assets/bin/Data/sharedassets0.assets'
        chunks = sorted((n for n in z.namelist() if n.startswith(entry + '.split')),
                        key=lambda n: int(n.rsplit('.split', 1)[1]))
        env = UnityPy.load(b''.join(z.read(n) for n in chunks))
        atlas = next(o.read().image for o in env.objects
                     if o.type.name == 'Texture2D' and o.read().m_Name == 'ui_title_Atlas').convert('RGBA')
        # The UIAtlas's NGUI sprite records store name + x/y/width/height in
        # top-left texture coordinates. Read them instead of guessing crops.
        record = next(o.get_raw_data() for o in env.objects
                      if o.type.name == 'MonoBehaviour' and b'ui_title_img_runners' in o.get_raw_data()
                      and b'ui_cmn_bg_sky4' in o.get_raw_data())
        names = {'sonic': 'ui_title_img_sonic', 'runners': 'ui_title_img_runners',
                 'revival': 'revival', 'base-white': 'ui_title_img_sonic_base_white',
                 'base-blue': 'ui_title_img_base_blue', 'star': 'ui_title_img_star'}
        crops, provenance, padding = {}, {}, {}
        for key, name in names.items():
            tag = name.encode()
            offset = record.index(struct.pack('<I', len(tag)) + tag) + 4 + len(tag)
            offset = (offset + 3) & ~3
            x, y, w, h, *extra = struct.unpack_from('<12i', record, offset)
            padding[key] = extra[4:8] # left/right/top/bottom after borders
            assert 0 <= x < x + w <= atlas.width and 0 <= y < y + h <= atlas.height
            crops[key] = atlas.crop((x, y, x + w, y + h))
            crops[key].save(source / (key + '.png'))
            provenance[key] = {'sprite': name, 'rect_xywh': [x, y, w, h], 'padding_lrtb': padding[key]}
        splash = next(o.read().image for o in env.objects
                      if o.type.name == 'Texture2D' and o.read().m_Name == 'Splash3').convert('RGBA')
        art_path = background or source / 'fan-background.png'
        splash = Image.open(art_path).convert('RGBA')
        splash.save(source / 'splash-background.png')
        game_art = build_game_splash(env, z, chunks, splash)
        scene = UnityPy.load(z.read('assets/bin/Data/mainData'))
        logo, layers = build_title_logo(scene, crops, padding, names)
    (source / 'sources.json').write_text(json.dumps({
        'apk_sha256': hashlib.sha256(Path(apk).read_bytes()).hexdigest(),
        'icon_entry': icon_entry, 'atlas_entry_parts': chunks,
        'atlas_texture': 'ui_title_Atlas', 'sprites': provenance,
        'background_texture': 'fan-background.png', 'game_splash': game_art, 'logo_scene': 'assets/bin/Data/mainData', 'logo_layers': layers,
    }, indent=2) + '\n')

    icon = Image.open(source / 'apk-icon.png').convert('RGBA')
    save8(icon.resize((128, 128), Image.Resampling.LANCZOS), ROOT / 'sce_sys/icon0.png')
    # Preserve the complete 16:9 artwork on both fixed-size Vita canvases.
    # Letterboxing is necessary: filling a taller canvas would crop/stretch.
    background = letterbox(splash, (840, 500))
    save8(letterbox(splash, (960, 544)), ROOT / 'sce_sys/pic0.png')
    save8(background, target / 'bg.png')

    logo.save(source / 'title-logo.png')
    startup = Image.new('RGBA', (280, 158))
    small = ImageOps.contain(logo, (268, 150), Image.Resampling.LANCZOS)
    startup.alpha_composite(small, ((280 - small.width) // 2, (158 - small.height) // 2))
    save8(startup, target / 'startup.png')

    # Actual-size composition for local visual QA (the system adds its UI).
    preview = background.copy()
    preview.alpha_composite(startup, ((840 - 280) // 2, (500 - 158) // 2))
    preview.save(ROOT / 'build/livearea-assets/livearea-preview.png')
    print('LiveArea and game splash: supplied fan artwork; round icon and original title logo preserved')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('apk', type=Path)
    parser.add_argument('--background', type=Path)
    args = parser.parse_args()
    main(args.apk, args.background)
