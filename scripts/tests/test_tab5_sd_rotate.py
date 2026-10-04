"""Tiled direct-output rotation is byte-identical to LVGL's 270-degree RGB565 rotate."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

BOARD = Path(__file__).resolve().parents[2] / "main/boards/qdtech/tab5"

SMOKE = r'''
#include <cassert>
#include <cstdio>
#include <vector>
#include "nabo_rotate.h"
// Reference: LVGL rotate270_rgb565 (src/draw/sw/lv_draw_sw_utils.c).
static void Lvgl(const uint16_t* src, uint16_t* dst, int w, int h, int ss, int ds) {
    for (int x = 0; x < w; ++x)
        for (int y = 0; y < h; ++y)
            dst[x * ds + (h - y - 1)] = src[y * ss + x];
}
int main() {
    const int sizes[][2] = {{434, 558}, {1, 1}, {31, 33}, {64, 64}, {65, 3}, {7, 100}};
    for (auto& s : sizes) {
        const int w = s[0], h = s[1], ss = w + 5, ds = h + 3;
        std::vector<uint16_t> src(size_t(ss) * h);
        for (size_t i = 0; i < src.size(); ++i) src[i] = uint16_t(i * 2654435761u >> 13);
        std::vector<uint16_t> a(size_t(ds) * w, 0xdead), b(size_t(ds) * w, 0xdead);
        Lvgl(src.data(), a.data(), w, h, ss, ds);
        nabo_sd::Rotate270Rgb565(src.data(), b.data(), w, h, ss, ds);
        assert(a == b);  // including untouched stride padding
    }
    std::puts("PASS tiled rotation matches LVGL rotate270_rgb565 for edge and frame sizes");
}
'''


class Tab5SdRotateTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_matches_lvgl(self):
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "rotate.cc"
            cpp.write_text(SMOKE)
            binary = Path(directory) / "rotate"
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(BOARD),
                            str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
