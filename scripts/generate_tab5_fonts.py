#!/usr/bin/env python3
"""Rebuild the Tab5 Chinese UI subsets from the licensed source font."""

import argparse
import hashlib
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
BOARD = ROOT / "main/boards/qdtech/tab5"
# Source: https://github.com/lxgw/LxgwWenKai/blob/main/fonts/TTF/LXGWWenKai-Regular.ttf
# Converter: lv_font_conv 1.5.3; license: BOARD / "FONT_LICENSE_OFL.txt".
FONT_SHA256 = "39ad71264b588165b469e35e6afb162a378dacd1f95348160240ba9038ac3009"
NATIVE_TITLE_SYMBOLS = "一东个中之乐交京亮体你值入具分前功动助北原台合吧听和国土在声外好始家对小就尿屏州工已幕广度开当录思我手择换控推播放数文新日是显智束来析果正每气氧江泵济珠生电白皮目示算米红经结络继绪续网置考聊肾能脉蛋血视觉触讯设话语说资轻输送选通道遥量闻静音频，"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("font", type=Path, help="LXGWWenKai-Regular.ttf from lxgw/LxgwWenKai")
    parser.add_argument("--converter", default="lv_font_conv")
    parser.add_argument("--sizes", nargs="+", type=int, choices=(16, 20, 28, 36),
                        default=(16, 20, 28, 36), help="Font sizes to regenerate")
    args = parser.parse_args()

    if hashlib.sha256(args.font.read_bytes()).hexdigest() != FONT_SHA256:
        parser.error("source font SHA-256 does not match the reviewed version")

    symbols = "".join(sorted(set((BOARD / "font_symbols.txt").read_text().strip())))
    for size in args.sizes:
        name = f"qd_font_lxgw_{size}"
        output = BOARD / f"{name}.c"
        subprocess.run(
            [
                args.converter,
                "--font", str(args.font),
                "--size", str(size),
                "--bpp", "4",
                "--format", "lvgl",
                "--lv-include", "lvgl.h",
                "--lv-font-name", name,
                "-r", "0x20-0x7F",
                "--symbols", symbols if size != 36 else NATIVE_TITLE_SYMBOLS,
                "-o", str(output),
            ],
            check=True,
        )
        source = output.read_text()
        lines = source.splitlines(keepends=True)
        lines = [
            " * Generated with scripts/generate_tab5_fonts.py; source: LXGW WenKai, SIL OFL 1.1.\n"
            if line.startswith(" * Opts:") else line
            for line in lines
        ]
        output.write_text("".join(lines))


if __name__ == "__main__":
    main()
