"""Game list titles from Chinese ROM file names."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

BOARD = Path(__file__).resolve().parents[2] / "main/boards/qdtech/tab5"

SMOKE = r'''
#include <cassert>
#include <cstdio>
#include "tab5_rom_names.h"
using tab5_roms::TidyTitle;
int main() {
    assert(TidyTitle("/sdcard/FC/106.冒险岛2无限人.nes") == "冒险岛2无限人");
    assert(TidyTitle("2.魂斗罗1代 无限人+散弹枪.nes") == "魂斗罗1代 无限人+散弹枪");
    assert(TidyTitle("235.超级魂斗罗_八花枪_(中文版).nes") == "超级魂斗罗 八花枪 (中文版)");
    assert(TidyTitle("56.洛克人6 HACK 版 .nes") == "洛克人6 HACK 版");
    assert(TidyTitle("吞食天地2 [先锋卡通汉化 (laopix简体中文名字版)].nes") == "吞食天地2");
    assert(TidyTitle("街霸60人加速版 [Cony Soft].NES") == "街霸60人加速版");
    assert(TidyTitle("07_战斧三代.bin") == "战斧三代");
    assert(TidyTitle("1942.nes") == "1942");          // a title that is only digits stays
    assert(TidyTitle("1943.nes") == "1943");
    assert(TidyTitle("3D.nes") == "3D");
    assert(TidyTitle("[Hack].nes") == "[Hack]");     // never empty
    assert(TidyTitle("雷鳥1.nes") == "雷鳥1");
    std::puts("PASS rom titles");
}
'''


class RomNamesTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_titles(self):
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "names.cc"
            cpp.write_text(SMOKE)
            binary = Path(directory) / "names"
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(BOARD), str(cpp),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
