#!/usr/bin/env python3
"""Regenerate the Tab5 split-flap clock numerals from Montserrat Bold."""

import argparse
import hashlib
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "main/boards/qdtech/tab5/qd_font_clock_72.c"
FONT_SHA256 = "06a0e623bbaf4a0237f1d605affa269f9e431ff50c7143dcbb47b815edaba9bd"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("font", type=Path, help="Montserrat-Bold.ttf from LVGL's test font files")
    parser.add_argument("--converter", default="lv_font_conv")
    args = parser.parse_args()
    if hashlib.sha256(args.font.read_bytes()).hexdigest() != FONT_SHA256:
        parser.error("source font SHA-256 does not match the reviewed version")

    subprocess.run([
        args.converter, "--font", str(args.font), "--size", "72", "--bpp", "4",
        "--format", "lvgl", "--lv-include", "lvgl.h",
        "--lv-font-name", "qd_font_clock_72", "-r", "0x2D-0x39",
        "-o", str(OUTPUT),
    ], check=True)
    lines = OUTPUT.read_text().splitlines(keepends=True)
    lines = [
        " * Generated with scripts/generate_tab5_clock_font.py; Montserrat Bold, SIL OFL 1.1.\n"
        if line.startswith(" * Opts:") else line
        for line in lines
    ]
    OUTPUT.write_text("".join(lines))


if __name__ == "__main__":
    main()
