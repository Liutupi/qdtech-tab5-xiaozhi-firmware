#!/usr/bin/env python3
"""Rebuild the Tab5 28 px font used for pushed text and home cards."""

import argparse
import hashlib
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
FONT_SHA256 = "39ad71264b588165b469e35e6afb162a378dacd1f95348160240ba9038ac3009"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("font", type=Path, help="LXGWWenKai-Regular.ttf from lxgw/LxgwWenKai")
    parser.add_argument("--converter", default="lv_font_conv")
    args = parser.parse_args()
    if hashlib.sha256(args.font.read_bytes()).hexdigest() != FONT_SHA256:
        parser.error("source font SHA-256 does not match the reviewed version")

    symbols = (ROOT / "scripts/tab5_dynamic_symbols.txt").read_text().strip()
    output = ROOT / "main/boards/qdtech/tab5/qd_font_cjk_28.c"
    subprocess.run(
        [args.converter, "--font", str(args.font), "--size", "28", "--bpp", "4",
         "--format", "lvgl", "--lv-include", "lvgl.h", "--lv-font-name",
         "qd_font_cjk_28", "-r", "0x20-0x7F", "--symbols", symbols,
         "-o", str(output)],
        check=True,
    )
    source = output.read_text()
    source = "\n".join(
        " * Generated from LXGW WenKai (SIL OFL 1.1) with lv_font_conv 1.5.3."
        if line.startswith(" * Opts:") else line
        for line in source.splitlines()
    ) + "\n"
    output.write_text(source)
    print(output)


if __name__ == "__main__":
    main()
