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
NATIVE_TITLE_SYMBOLS = "小智对话轻触开始结束语音继续正在思考听你说来聊聊吧视觉和声音已就绪原生显示我是好，土皮助手设置网络电台屏幕亮度播放音量目录当前中国之声北京新闻广播交通广州资讯广东珠江经济文体动音乐频道选择一个肾功能尿白蛋氧合血气分析静脉泵数值工具输入换算结果"


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
