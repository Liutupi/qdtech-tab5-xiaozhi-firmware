#!/usr/bin/env python3
"""Generate the optional uncompressed Tab5 Think torso from Nabo source art.

The output has the same RGB565 plane and alpha plane as the LZ4 image in
``nabo_assets.c``. Refuse stale source art rather than silently changing the
experimental pose.
"""

import argparse
from pathlib import Path
import re
import struct

from nabo_image_codec import lz4_block, rgb565a8
from prepare_nabo_assets import reaction_rigs


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "assets/nabo/think_torso_262x462_rgb565a8.bin"
WIDTH, HEIGHT = 262, 462
EMBEDDED_NAME = "nabo_think_torso_data"


def embedded_lz4(root=ROOT):
    source = (root / "main/boards/qdtech/tab5/nabo_assets.c").read_text()
    match = re.search(r"static const uint8_t " + EMBEDDED_NAME +
                      r"\[\].*?= \{(.*?)\};", source, re.S)
    if not match:
        raise ValueError("Generated Think torso was not found in nabo_assets.c")
    return bytes(int(part, 16) for part in re.findall(r"0x([0-9a-fA-F]{2})", match[1]))


def prepare(root=ROOT):
    image = reaction_rigs()["think"][0]["torso"]
    if image.size != (WIDTH, HEIGHT):
        raise ValueError(f"Think torso geometry changed: {image.size}")
    raw = rgb565a8(image)
    embedded = embedded_lz4(root)
    compressed = lz4_block(raw)
    expected = struct.pack("<III", 2, len(compressed), len(raw)) + compressed
    if embedded != expected:
        raise ValueError("Nabo source art differs from the generated LZ4 Think torso")
    return raw


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    args = parser.parse_args()
    data = prepare()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    print(f"Wrote {len(data)} bytes to {args.output}")


if __name__ == "__main__":
    main()
