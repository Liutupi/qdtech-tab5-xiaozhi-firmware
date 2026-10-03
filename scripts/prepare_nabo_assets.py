#!/usr/bin/env python3
"""Build Tab5-native Nabo art from the owner's transparent source sheets.

The resting portrait stays still. Blink patches cover only the eyes, so the
display driver rotates and refreshes a small rectangle during idle animation.
"""

from io import BytesIO
from pathlib import Path
from collections import deque
import json
import struct
from nabo_image_codec import rgb565a8, lz4_block

from PIL import Image, ImageChops, ImageDraw, ImageFilter


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "assets/nabo/source"
OUTPUT = ROOT / "main/boards/qdtech/tab5"


def clean_sprite(frame: Image.Image) -> Image.Image:
    alpha = frame.getchannel("A")
    original = alpha.tobytes()
    visited = bytearray(len(original))
    largest = []
    for start, value in enumerate(original):
        if value < 16 or visited[start]:
            continue
        component = []
        pending = deque([start])
        visited[start] = 1
        while pending:
            pixel = pending.popleft()
            component.append(pixel)
            x, y = pixel % frame.width, pixel // frame.width
            for adjacent in (pixel - 1 if x else -1,
                             pixel + 1 if x + 1 < frame.width else -1,
                             pixel - frame.width if y else -1,
                             pixel + frame.width if y + 1 < frame.height else -1):
                if adjacent >= 0 and not visited[adjacent] and original[adjacent] >= 16:
                    visited[adjacent] = 1
                    pending.append(adjacent)
        if len(component) > len(largest):
            largest = component
    cleaned = bytearray(len(original))
    for pixel in largest:
        cleaned[pixel] = original[pixel]
    frame.putalpha(Image.frombytes("L", frame.size, bytes(cleaned)))
    return frame


def sprite_frames(path: Path, columns: int = 4, rows: int = 2) -> list[Image.Image]:
    """Slice a generated sheet with one common canvas and baseline for all frames.

    Keep the connected character alpha and discard detached reference-sheet
    flecks. Never resize individual frames to their own bounds: that creates
    visible head/foot jumps when the animation plays.
    """
    sheet = Image.open(path).convert("RGBA")
    frames = []
    for row in range(rows):
        for column in range(columns):
            frame = sheet.crop((round(column * sheet.width / columns),
                                round(row * sheet.height / rows),
                                round((column + 1) * sheet.width / columns),
                                round((row + 1) * sheet.height / rows)))
            frame = clean_sprite(frame)
            frames.append(frame)
    bounds = [frame.getbbox() for frame in frames]
    if any(bounds is None for bounds in bounds):
        raise ValueError(f"Empty sprite in {path}")
    union = (min(b[0] for b in bounds), min(b[1] for b in bounds),
             max(b[2] for b in bounds), max(b[3] for b in bounds))
    scale = min(300 / (union[2] - union[0]), 561 / (union[3] - union[1]))
    size = (round((union[2] - union[0]) * scale), round((union[3] - union[1]) * scale))
    result = []
    for frame, bounds in zip(frames, bounds):
        sprite = frame.crop(union).resize(size, Image.Resampling.LANCZOS)
        canvas = Image.new("RGBA", (300, 561))
        # Align each actual shoe baseline; retain one shared scale and x axis.
        baseline_offset = round((union[3] - bounds[3]) * scale)
        canvas.alpha_composite(sprite, ((300 - size[0]) // 2, 561 - size[1] + baseline_offset))
        result.append(canvas)
    return result


def panel_frame(image: Image.Image) -> Image.Image:
    """Prepare RGB for the Tab5's RGB565 panel; preserve the alpha channel."""
    red, green, blue, alpha = image.split()
    return Image.merge("RGBA", (red.point(lambda value: value & 0xF8),
                                green.point(lambda value: value & 0xFC),
                                blue.point(lambda value: value & 0xF8), alpha))


def wave_puppet() -> tuple[Image.Image, Image.Image, Image.Image, dict]:
    sheet = Image.open(SOURCE / "wave-rig-v3.png").convert("RGBA")
    metadata = json.loads((SOURCE / "wave-rig-v3.json").read_text())
    wrist = json.loads((SOURCE / "wave-hand-v4.json").read_text())
    hand = Image.open(SOURCE / "wave-hand-v4.png").convert("RGBA")
    if list(sheet.size) != metadata["sheet_size"]:
        raise ValueError("Wave rig sheet size changed; review its joint registration")
    if list(hand.size) != wrist["image_size"]:
        raise ValueError("Wave hand size changed; review its wrist registration")
    width = sheet.width // 2
    base = clean_sprite(sheet.crop((0, 0, width, sheet.height)))
    hand = clean_sprite(hand)
    bounds = base.getbbox()
    hand_bounds = hand.getbbox()
    if bounds is None or hand_bounds is None:
        raise ValueError("Wave rig has an empty layer")
    scale = min(300 / (bounds[2] - bounds[0]), 561 / (bounds[3] - bounds[1]))
    size = (round((bounds[2] - bounds[0]) * scale), round((bounds[3] - bounds[1]) * scale))
    origin = ((300 - size[0]) // 2, 561 - size[1])
    body = Image.new("RGBA", (300, 561))
    body.alpha_composite(base.crop(bounds).resize(size, Image.Resampling.LANCZOS), origin)
    # The base already contains the entire sleeve. Attach only a short wrist
    # and hand; sizing against the removed forearm would lengthen the arm.
    hand_scale = wrist["target_width"] / (hand_bounds[2] - hand_bounds[0])
    hand_size = (wrist["target_width"],
                 round((hand_bounds[3] - hand_bounds[1]) * hand_scale))
    hand = hand.crop(hand_bounds).resize(hand_size, Image.Resampling.LANCZOS)
    pivot = (round((wrist["hand_pivot"][0] - hand_bounds[0]) * hand_scale),
             round((wrist["hand_pivot"][1] - hand_bounds[1]) * hand_scale))
    joint = (origin[0] + round((wrist["base_wrist"][0] - bounds[0]) * scale),
             origin[1] + round((wrist["base_wrist"][1] - bounds[1]) * scale))
    placement = {"x": joint[0] - pivot[0], "y": joint[1] - pivot[1],
                 "pivot_x": pivot[0], "pivot_y": pivot[1],
                 "rest_angle": wrist["rest_angle"],
                 "cuff_x": wrist["cuff_bounds"][0], "cuff_y": wrist["cuff_bounds"][1]}
    # A fixed foreground lip occludes the wrist while the rear cuff stays
    # behind it. Both sleeve layers come from the unchanged registered body.
    mask = Image.new("L", body.size)
    ImageDraw.Draw(mask).polygon(wrist["cuff_front"], fill=255)
    cuff = body.copy()
    cuff.putalpha(Image.composite(body.getchannel("A"), Image.new("L", body.size), mask))
    cuff = cuff.crop(tuple(wrist["cuff_bounds"]))
    return panel_frame(body), panel_frame(hand), panel_frame(cuff), placement


def png_bytes(image: Image.Image) -> bytes:
    out = BytesIO()
    image.save(out, format="PNG", optimize=True)
    return out.getvalue()


def wave_rig() -> tuple[dict, dict]:
    """Register existing artwork to hips/neck/wrist without adding anatomy."""
    body, hand, cuff, wrist = wave_puppet()
    registration = json.loads((SOURCE / "wave-rig-v5.json").read_text())
    if list(body.size) != registration["canvas_size"]:
        raise ValueError("Wave body size changed; review the rig masks")
    head_mask = Image.new("L", body.size)
    ImageDraw.Draw(head_mask).polygon(registration["head_outline"], fill=255)
    torso_mask = head_mask.point(lambda value: 255 - value)
    ImageDraw.Draw(torso_mask).polygon(registration["neck_overlap"], fill=255)
    ImageDraw.Draw(torso_mask).rectangle((0, registration["torso_bottom"], 300, 561), fill=0)
    leg_mask = Image.new("L", body.size)
    draw = ImageDraw.Draw(leg_mask)
    draw.polygon(registration["left_leg"], fill=255)
    draw.polygon(registration["right_leg"], fill=255)
    draw.rectangle((0, registration["lower_legs_top"], 300, 561), fill=255)
    layers, geometry = {}, {}
    for name, mask, pivot in (("legs", leg_mask, [150, 561]),
                               ("torso", torso_mask, registration["hip"]),
                               ("head", head_mask, registration["neck"])):
        image = body.copy()
        image.putalpha(Image.composite(body.getchannel("A"), Image.new("L", body.size), mask))
        bounds = image.getbbox()
        if bounds is None:
            raise ValueError(f"Empty wave {name} layer")
        layers[name] = image.crop(bounds)
        geometry[name] = {"x": bounds[0], "y": bounds[1],
                          "pivot_x": pivot[0] - bounds[0], "pivot_y": pivot[1] - bounds[1]}
    layers.update(hand=hand, cuff=cuff)
    geometry["hand"] = {key: wrist[key] for key in ("x", "y", "pivot_x", "pivot_y")}
    geometry["cuff"] = {"x": wrist["cuff_x"], "y": wrist["cuff_y"],
                        "pivot_x": wrist["x"] + wrist["pivot_x"] - wrist["cuff_x"],
                        "pivot_y": wrist["y"] + wrist["pivot_y"] - wrist["cuff_y"]}
    geometry["hand_rest_angle"] = wrist["rest_angle"]
    return layers, geometry


def reaction_rigs(frames=None) -> dict:
    """Split existing reaction poses at their own neck and hip registrations."""
    registration = json.loads((SOURCE / "reactions-rig-v7.json").read_text())
    if frames is None:
        frames = [panel_frame(frame) for frame in sprite_frames(SOURCE / "reactions-v2.png")]
    result = {}
    for name, pose in registration["poses"].items():
        body = frames[pose["frame"]]
        if list(body.size) != registration["canvas_size"]:
            raise ValueError(f"Reaction {name} size changed; review joint registration")
        head_mask = Image.new("L", body.size)
        if not pose.get("head_follows_torso"):
            ImageDraw.Draw(head_mask).polygon(pose["head_outline"], fill=255)
        leg_mask = Image.new("L", body.size)
        draw = ImageDraw.Draw(leg_mask)
        for polygon in pose["leg_polygons"]:
            draw.polygon(polygon, fill=255)
        lower_top = 490 if name == "listen" else 500
        draw.rectangle((0, lower_top, 300, 561), fill=255)
        # Keep pale coat hems with the moving torso. The leg polygons only
        # select the dark navy fabric above the exposed ankles and shoes.
        pixels, selected = body.load(), leg_mask.load()
        for y in range(lower_top):
            for x in range(body.width):
                if selected[x, y] and max(pixels[x, y][:3]) > 120:
                    selected[x, y] = 0
        hand_mask = Image.new("L", body.size)
        cuff_mask = Image.new("L", body.size)
        if "hand_outline" in pose:
            ImageDraw.Draw(hand_mask).polygon(pose["hand_outline"], fill=255)
            hand_image = body.copy()
            hand_image.putalpha(ImageChops.multiply(body.getchannel("A"), hand_mask))
            hand_image = clean_sprite(hand_image)
            # Detached head/ear pixels stay in the original upper-body layer.
            hand_mask = hand_image.getchannel("A").point(lambda a: 255 if a else 0)
            ImageDraw.Draw(cuff_mask).polygon(pose["cuff_front"], fill=255)
        torso_mask = ImageChops.invert(ImageChops.lighter(
            ImageChops.lighter(head_mask, leg_mask), hand_mask))
        draw = ImageDraw.Draw(torso_mask)
        # Overlapping navy fabric keeps the hip covered during small bounces.
        overlap = leg_mask.copy()
        ImageDraw.Draw(overlap).rectangle((0, 453, 300, 561), fill=0)
        torso_mask = ImageChops.lighter(torso_mask, overlap)
        if not pose.get("head_follows_torso"):
            ImageDraw.Draw(torso_mask).polygon(pose["neck_overlap"], fill=255)
        layers, geometry = {}, {}
        for part, mask, pivot in (("legs", leg_mask, [150, 561]),
                                  ("torso", torso_mask, pose["hip"]),
                                  ("head", head_mask, pose["neck"])):
            layer = body.copy()
            layer.putalpha(ImageChops.multiply(body.getchannel("A"), mask))
            bounds = layer.getbbox()
            if bounds is None and part == "head" and pose.get("head_follows_torso"):
                geometry[part] = {"x":0, "y":0, "pivot_x":0, "pivot_y":0}
                continue
            if bounds is None:
                raise ValueError(f"Empty {name} {part} layer")
            layers[part] = layer.crop(bounds)
            geometry[part] = {"x": bounds[0], "y": bounds[1],
                              "pivot_x": pivot[0] - bounds[0], "pivot_y": pivot[1] - bounds[1]}
        geometry.update(hand={"x":0, "y":0, "pivot_x":0, "pivot_y":0},
                        cuff={"x":0, "y":0, "pivot_x":0, "pivot_y":0}, hand_rest_angle=0)
        if "hand_outline" in pose:
            for part, mask in (("hand", hand_mask), ("cuff", cuff_mask)):
                layer = body.copy()
                layer.putalpha(ImageChops.multiply(body.getchannel("A"), mask))
                bounds = layer.getbbox()
                if bounds is None:
                    raise ValueError(f"Empty {name} {part} layer")
                layers[part] = layer.crop(bounds)
                geometry[part] = {"x":bounds[0], "y":bounds[1],
                                  "pivot_x":pose["wrist"][0] - bounds[0],
                                  "pivot_y":pose["wrist"][1] - bounds[1]}
        result[name] = (layers, geometry)
    return result


def rig_initializer(geometry: dict) -> str:
    parts = ["{" + ", ".join(str(geometry[name][key]) for key in
                              ("x", "y", "pivot_x", "pivot_y")) + "}"
             for name in ("legs", "torso", "head", "hand", "cuff")]
    return "{" + ", ".join(parts + [str(geometry["hand_rest_angle"])]) + "}"


def image_definition(name: str, image: Image.Image, raw: bool = False, lz4: bool = False) -> str:
    if raw or lz4:
        data = rgb565a8(image)
        if lz4:
            compressed = lz4_block(data)
            data = struct.pack("<III", 2, len(compressed), len(data)) + compressed
    else:
        data = png_bytes(image)
    lines = [f"static const uint8_t {name}_data[] __attribute__((aligned(4))) = {{"]
    for start in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in data[start : start + 16]) + ",")
    lines += [
        "};",
        f"const lv_image_dsc_t {name} = {{",
        "    .header.magic = LV_IMAGE_HEADER_MAGIC,",
        f"    .header.cf = {'LV_COLOR_FORMAT_RGB565A8' if raw or lz4 else 'LV_COLOR_FORMAT_RAW_ALPHA'},",
        *(["    .header.flags = LV_IMAGE_FLAGS_COMPRESSED,"] if lz4 else []),
        *([f"    .header.stride = {image.width * 2},"] if raw or lz4 else []),
        f"    .header.w = {image.width},",
        f"    .header.h = {image.height},",
        f"    .data_size = sizeof({name}_data),",
        f"    .data = {name}_data,",
        "};",
        "",
    ]
    print(f"{name}: {image.width}x{image.height}, {len(data)} {'LZ4 RGB565A8' if lz4 else 'raw' if raw else 'PNG'} bytes")
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

    # Static art draws directly from RGB565A8 instead of keeping an ARGB PNG copy in PSRAM.
    definitions = [image_definition("nabo_portrait", portrait, raw=True)]
    for frame, name in ((1, "nabo_half_blink"), (2, "nabo_closed_blink")):
        x0 = round(frame * eyes.width / 5)
        x1 = round((frame + 1) * eyes.width / 5)
        alternative = eyes.crop((x0, 0, x1, eyes.height)).resize((434, 724))
        patch = alternative.crop(region)
        patch.putalpha(Image.composite(patch.getchannel("A"), Image.new("L", patch.size), mask.crop(region)))
        definitions.append(image_definition(name, patch, raw=True))

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
    definitions.append(image_definition("nabo_sleep", sleep, raw=True))

    preview_frames = []
    preview_dir = ROOT / "assets/nabo/frames-v2"
    preview_dir.mkdir(exist_ok=True)
    for sheet_name, prefix in (("wave-v2.png", "nabo_wave_v2"),
                               ("reactions-v2.png", "nabo_reaction")):
        frames = [panel_frame(frame) for frame in sprite_frames(SOURCE / sheet_name)]
        for index, frame in enumerate(frames):
            definitions.append(image_definition(f"{prefix}_{index}", frame))
            frame.save(preview_dir / f"{prefix}-{index}.png")
        preview_frames.extend(frames)
        definitions.append(f"const lv_image_dsc_t* const {prefix}_frames[8] = {{\n    " +
                           ", ".join(f"&{prefix}_{i}" for i in range(8)) + "\n};\n")

    preview = Image.new("RGB", (1200, 640), (17, 40, 66))
    for index, frame in enumerate(preview_frames):
        thumbnail = frame.resize((150, 280), Image.Resampling.LANCZOS)
        x, y = (index % 8) * 150, (index // 8) * 320
        preview.paste(thumbnail, (x, y + 10), thumbnail)
        ImageDraw.Draw(preview).text((x + 20, y + 300), str(index % 8 + 1), fill="white")
    preview.save(ROOT / "assets/nabo/preview-v2.png")
    sequence = (0, 1, 2, 3, 4, 5, 4, 5, 4, 5, 6, 7, 0)
    animation = []
    for index in sequence:
        canvas = Image.new("RGB", (340, 590), (17, 40, 66))
        canvas.paste(preview_frames[index], (20, 15), preview_frames[index])
        animation.append(canvas)
    animation[0].save(ROOT / "assets/nabo/wave-v2-preview.gif", save_all=True,
                      append_images=animation[1:], duration=100, loop=0)

    layers, geometry = wave_rig()
    for name, image in layers.items():
        definitions.append(image_definition(f"nabo_wave_{name}", image, raw=name != "legs"))
    from preview_nabo_greeting import render_preview
    render_preview(ROOT, layers, geometry)

    reactions = reaction_rigs(preview_frames[8:])
    for reaction, (parts, _) in reactions.items():
        for name, image in parts.items():
            definitions.append(image_definition(f"nabo_{reaction}_{name}", image,
                lz4=reaction in ("think", "wink", "encourage", "curious", "comfort")
                and name == "torso"))
    from preview_nabo_reactions import render_reactions
    render_reactions(ROOT, {name: reactions[name] for name in
                            ("listen", "think", "happy", "music")}, version=6)
    render_reactions(ROOT, {name: reactions[name] for name in
                            ("wink", "encourage", "curious", "comfort")}, version=7)

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
extern const lv_image_dsc_t nabo_wave_legs;
extern const lv_image_dsc_t nabo_wave_torso;
extern const lv_image_dsc_t nabo_wave_head;
extern const lv_image_dsc_t nabo_wave_hand;
extern const lv_image_dsc_t nabo_wave_cuff;
extern const lv_image_dsc_t nabo_mouth_half;
extern const lv_image_dsc_t nabo_mouth_open;
extern const lv_image_dsc_t* const nabo_wave_v2_frames[8];
extern const lv_image_dsc_t* const nabo_reaction_frames[8];
extern const unsigned char nabo_greeting_ogg[];
extern const unsigned int nabo_greeting_ogg_len;
#ifdef __cplusplus
}
#endif
"""
    declarations = "".join(f"extern const lv_image_dsc_t nabo_{reaction}_{part};\n"
                           for reaction, (parts, _) in reactions.items() for part in parts)
    header = header.replace('extern const lv_image_dsc_t nabo_mouth_half;',
                            declarations + 'extern const lv_image_dsc_t nabo_mouth_half;')
    header += ('\n#ifdef __cplusplus\n#include "nabo_animation.h"\n'
               'inline constexpr nabo::WaveGeometry NaboWaveGeometry() {\n    return ' +
               rig_initializer(geometry) + ';\n}\n#endif\n')
    rig_header = """struct NaboReactionLayers {
    const lv_image_dsc_t *legs, *torso, *head, *hand, *cuff;
    nabo::WaveGeometry geometry;
};
inline constexpr NaboReactionLayers NaboReactionRig(nabo::Action action) {
    switch (action) {
"""
    for reaction, (parts, registration) in reactions.items():
        sources = ", ".join(f"&nabo_{reaction}_{part}" if part in parts else "nullptr"
                            for part in ("legs", "torso", "head", "hand", "cuff"))
        rig_header += (f"    case nabo::Action::{reaction.title()}:\n"
                       f"        return {{{sources}, {rig_initializer(registration)}}};\n")
    rig_header += "    default: return {nullptr, nullptr, nullptr, nullptr, nullptr, {}};\n    }\n}\n"
    header = header.rsplit("#endif", 1)[0] + rig_header + "#endif\n"
    # Replace completed outputs atomically if a build is reading the assets.
    for name, source in (("nabo_assets.h", header),
                         ("nabo_assets.c", '#include "nabo_assets.h"\n\n#include <stdint.h>\n\n' +
                          "\n".join(definitions))):
        temporary = OUTPUT / (name + ".tmp")
        temporary.write_text(source)
        temporary.replace(OUTPUT / name)


if __name__ == "__main__":
    main()
