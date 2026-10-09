"""MD game catalog (catalog.tsv from the earlier ESP32 board)."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

BOARD = Path(__file__).resolve().parents[2] / "main/boards/qdtech/tab5"

SMOKE = r'''
#include <cassert>
#include <cstdio>
#include "tab5_md_catalog.h"
using namespace tab5_md;
int main() {
    const char* tsv =
        "\xEF\xBB\xBF" "filename\ttitle\tcategory\r\n"
        "01_索尼克一代.bin\t索尼克一代\t动作\r\n"
        "48_超级街头霸王二简体版.bin\t超级街头霸王二简体版\t格斗\n"
        "../escape.bin\tbad\tx\n"
        "readme.txt\tnot a rom\tx\n"
        "50_快打旋风二无敌版.BIN\n"
        "\n";
    Catalog c = ParseCatalog(tsv);
    assert(c.size() == 3);
    assert(c[0].file == "01_索尼克一代.bin" && c[0].title == "索尼克一代" && c[0].category == "动作");
    assert(c[1].category == "格斗");
    assert(c[2].file == "50_快打旋风二无敌版.BIN" && c[2].title == "快打旋风二无敌版");  // no title column
    assert(IsRomFile("x.gen") && IsRomFile("x.smd") && IsRomFile("x.md") && !IsRomFile("x.nes") &&
           !IsRomFile("a/b.bin") && !IsRomFile(".bin"));
    std::puts("PASS md catalog");
}
'''


class MdCatalogTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_catalog(self):
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "md.cc"
            cpp.write_text(SMOKE)
            binary = Path(directory) / "md"
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(BOARD), str(cpp),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
