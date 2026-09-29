#!/usr/bin/env python3
"""Build Tab5-native Nabo art from the owner's transparent source sheets.

The resting portrait stays still. Blink patches cover only the eyes, so the
display driver rotates and refreshes a small rectangle during idle animation.
"""

from io import BytesIO
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "assets/nabo/source"
OUTPUT = ROOT / "main/boards/qdtech/tab5"


def png_bytes(image: Image.Image) -> bytes:
    out = BytesIO()
    image.save(out, format="PNG", optimize=True)
    return out.getvalue()


def image_definition(name: str, image: Image.Image) -> str:
    data = png_bytes(image)
    lines = [f"static const uint8_t {name}_data[] __attribute__((aligned(4))) = {{"]
    for start in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in data[start : start + 16]) + ",")
    lines += [
        "};",
        f"const lv_image_dsc_t {name} = {{",
        "    .header.magic = LV_IMAGE_HEADER_MAGIC,",
        "    .header.cf = LV_COLOR_FORMAT_RAW_ALPHA,",
        f"    .header.w = {image.width},",
        f"    .header.h = {image.height},",
        f"    .data_size = sizeof({name}_data),",
        f"    .data = {name}_data,",
        "};",
        "",
    ]
    print(f"{name}: {image.width}x{image.height}, {len(data)} PNG bytes")
    return "\n".join(lines)


def main() -> None:
    eyes = Image.open(SOURCE / "eyes.png").convert("RGBA")
    portrait = eyes.crop((0, 64, 434, 682))
    region = (65, 310, 377, 435)

    mask = Image.new("L", eyes.size)
    draw = ImageDraw.Draw(mask)
    draw.rounded_rectangle((80, 335, 218, 428), radius=35, fill=255)
    draw.rounded_rectangle((221, 334, 360, 429), radius=35, fill=255)
    mask = mask.filter(ImageFilter.GaussianBlur(8))

    definitions = [image_definition("nabo_portrait", portrait)]
    for frame, name in ((1, "nabo_half_blink"), (2, "nabo_closed_blink")):
        x0 = round(frame * eyes.width / 5)
        x1 = round((frame + 1) * eyes.width / 5)
        alternative = eyes.crop((x0, 0, x1, eyes.height)).resize((434, 724))
        patch = alternative.crop(region)
        patch.putalpha(Image.composite(patch.getchannel("A"), Image.new("L", patch.size), mask.crop(region)))
        definitions.append(image_definition(name, patch))

    wave_sheet = Image.open(SOURCE / "wave.png").convert("RGBA")
    # Three matching standing poses form a brief greeting. Keep them at native
    # display size; the home portrait is never transformed each frame.
    for column, name in ((1, "nabo_wave_low"), (2, "nabo_wave"),
                         (3, "nabo_wave_side")):
        # The last pose holds its hand farther left; include the whole palm.
        x = 1650 if column == 3 else column * 543 + 61
        wave = wave_sheet.crop((x, 18, x + 366, 702))
        wave = wave.resize((300, 561), Image.Resampling.LANCZOS)
        definitions.append(image_definition(name, wave))

    sleep_sheet = Image.open(SOURCE / "poses.png").convert("RGBA")
    sleep = sleep_sheet.crop((20, 337, 623, 665))
    sleep = sleep.resize((480, 261), Image.Resampling.LANCZOS)
    definitions.append(image_definition("nabo_sleep", sleep))

    guide = Image.open(SOURCE / "reference.png").convert("RGBA")
    # The expression board supplies mouth shapes on a peach tile. Match its
    # skin tone to the portrait, then feather the edge over the resting mouth.
    for name, center_x in (("nabo_mouth_half", 129), ("nabo_mouth_open", 205)):
        mouth = guide.crop((center_x - 24, 497, center_x + 24, 528))
        pixels = mouth.load()
        for y in range(mouth.height):
            for x in range(mouth.width):
                red, green, blue, alpha = pixels[x, y]
                pixels[x, y] = (max(0, red - 1), max(0, green - 17),
                                max(0, blue - 20), alpha)
        mouth_mask = Image.new("L", mouth.size)
        draw = ImageDraw.Draw(mouth_mask)
        draw.rounded_rectangle((3, 2, 45, 29), radius=13, fill=255)
        mouth_mask = mouth_mask.filter(ImageFilter.GaussianBlur(3))
        mouth.putalpha(Image.composite(mouth.getchannel("A"),
                                       Image.new("L", mouth.size), mouth_mask))
        definitions.append(image_definition(name, mouth))

    greeting = (ROOT / "assets/nabo/greeting.ogg").read_bytes()
    greeting_lines = ["const unsigned char nabo_greeting_ogg[] = {"]
    for start in range(0, len(greeting), 16):
        greeting_lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in greeting[start:start + 16]) + ",")
    greeting_lines += ["};", f"const unsigned int nabo_greeting_ogg_len = {len(greeting)};", ""]
    definitions.append("\n".join(greeting_lines))

    header = """#pragma once
#include \"lvgl.h\"

#ifdef __cplusplus
extern \"C\" {
#endif
extern const lv_image_dsc_t nabo_portrait;
extern const lv_image_dsc_t nabo_half_blink;
extern const lv_image_dsc_t nabo_closed_blink;
extern const lv_image_dsc_t nabo_wave_low;
extern const lv_image_dsc_t nabo_wave;
extern const lv_image_dsc_t nabo_wave_side;
extern const lv_image_dsc_t nabo_sleep;
extern const lv_image_dsc_t nabo_mouth_half;
extern const lv_image_dsc_t nabo_mouth_open;
extern const unsigned char nabo_greeting_ogg[];
extern const unsigned int nabo_greeting_ogg_len;
#ifdef __cplusplus
}
#endif
"""
    (OUTPUT / "nabo_assets.h").write_text(header)
    (OUTPUT / "nabo_assets.c").write_text(
        '#include "nabo_assets.h"\n\n#include <stdint.h>\n\n' + "\n".join(definitions)
    )


if __name__ == "__main__":
    main()
