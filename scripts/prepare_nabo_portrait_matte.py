#!/usr/bin/env python3
"""Generate the opt-in Tab5 RGB565 portrait from the current Nabo source.

The 4x Pillow rounded rectangle approximates LVGL's card mask. This is an
A/B asset, not a pixel-exact render capture. It must be checked on the panel.
"""

import argparse
import re
from pathlib import Path

from PIL import Image, ImageDraw

from nabo_image_codec import rgb565a8


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "assets/nabo/portrait_matte_434x558_rgb565.bin"
WIDTH, HEIGHT = 434, 558
PORTRAIT_X = 39
OUTER_FILL, OUTER_BORDER, INNER_FILL = 0x112842, 0x285272, 0x183550
INNER_X, INNER_Y, INNER_W, INNER_H = 42, 28, 422, 492
INNER_RADIUS = 211  # LVGL clamps the configured 220 to half of width 422.


def rgb565(color):
    if isinstance(color, int):
        color = ((color >> 16) & 255, (color >> 8) & 255, color & 255)
    r, g, b = color
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def mix565(fg, bg, alpha):
    """Mirror LVGL 9's lv_color_16_16_mix integer arithmetic."""
    if alpha == 255:
        return fg
    if alpha == 0 or fg == bg:
        return bg
    mix = (alpha + 4) >> 3
    mask = 0x7E0F81F
    bg32 = (bg | (bg << 16)) & mask
    fg32 = (fg | (fg << 16)) & mask
    product = ((fg32 - bg32) * mix) & 0xFFFFFFFF
    result = (((product >> 5) + bg32) & 0xFFFFFFFF) & mask
    return ((result >> 16) | result) & 0xFFFF


def portrait_source(root=ROOT):
    eyes = Image.open(root / "assets/nabo/source/eyes.png").convert("RGBA")
    full = eyes.crop((0, 64, 434, 682))
    asset = (root / "main/boards/qdtech/tab5/nabo_assets.c").read_text()
    begin = asset.index("static const uint8_t nabo_portrait_data[]")
    end = asset.index("};", begin)
    embedded = bytes(int(part, 16) for part in re.findall(r"0x([0-9a-fA-F]{2})", asset[begin:end]))
    if embedded != rgb565a8(full):
        raise ValueError("eyes.png no longer matches nabo_assets.c; regenerate base art first")
    return full.crop((0, 0, WIDTH, HEIGHT))


def inner_coverage():
    scale = 4
    canvas = Image.new("L", (512 * scale, HEIGHT * scale), 0)
    ImageDraw.Draw(canvas).rounded_rectangle(
        (INNER_X * scale, INNER_Y * scale,
         (INNER_X + INNER_W) * scale - 1, (INNER_Y + INNER_H) * scale - 1),
        radius=INNER_RADIUS * scale, fill=255)
    return canvas.resize((512, HEIGHT), Image.Resampling.BOX).crop(
        (PORTRAIT_X, 0, PORTRAIT_X + WIDTH, HEIGHT)).tobytes()


def compose(root=ROOT):
    source = portrait_source(root).tobytes()
    coverage = inner_coverage()
    outer, border, inner = map(rgb565, (OUTER_FILL, OUTER_BORDER, INNER_FILL))
    result = bytearray(WIDTH * HEIGHT * 2)
    for index, cover in enumerate(coverage):
        y = index // WIDTH
        base = border if y in (0, HEIGHT - 1) else outer
        bg = mix565(inner, base, cover)
        offset = index * 4
        fg = rgb565(source[offset:offset + 3])
        pixel = mix565(fg, bg, source[offset + 3])
        result[index * 2:index * 2 + 2] = pixel.to_bytes(2, "little")
    return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    args = parser.parse_args()
    data = compose()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    print(f"Wrote {len(data)} bytes to {args.output}")


if __name__ == "__main__":
    main()
