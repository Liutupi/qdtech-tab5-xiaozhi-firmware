#!/usr/bin/env python3
"""Build the native NABO podcast's 48 px WenKai lyric font without SD changes."""

import argparse
import hashlib
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# Same reviewed WenKai source as the SD radio artwork project (SIL OFL 1.1).
FONT_SHA256 = "bf73f995c05f3f7baf7840b9d76d8d9678c8b150a020946a394ca63c67ed1350"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("font", type=Path)
    parser.add_argument("--converter", default="lv_font_conv")
    args = parser.parse_args()
    if hashlib.sha256(args.font.read_bytes()).hexdigest() != FONT_SHA256:
        parser.error("source font SHA-256 does not match the reviewed version")
    output = ROOT / "main/boards/qdtech/tab5/qd_font_lyric_48.c"
    # A larger one-bit raster keeps broad CJK coverage inside the 12 MiB factory app.
    # Rare characters retain the pinned full text-font fallback on the page.
    subprocess.run([
        args.converter, "--font", str(args.font), "--size", "48", "--bpp", "1",
        "--format", "lvgl", "--lv-include", "lvgl.h", "--lv-font-name", "qd_font_lyric_48",
        "-r", "0x20-0x7F", "--symbols", (ROOT / "scripts/tab5_dynamic_symbols.txt").read_text().strip(),
        "-o", str(output),
    ], check=True)
    source = output.read_text()
    output.write_text("\n".join(
        " * Generated with scripts/generate_tab5_lyric_font.py; LXGW WenKai, SIL OFL 1.1."
        if line.startswith(" * Opts:") else line for line in source.splitlines()
    ).rstrip() + "\n")
    print(output)


if __name__ == "__main__":
    main()
