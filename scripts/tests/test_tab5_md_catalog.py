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


    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_launcher_requires_controls_audio_version(self):
        source = (BOARD / "tab5_md_launcher.cc").read_text()
        guard = "bool MdAppPresent(" + source.split("bool MdAppPresent(", 1)[1].split("\n}", 1)[0] + "\n}"
        harness = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
struct esp_partition_t {};
struct esp_app_desc_t { char project_name[32]; char version[32]; };
constexpr int ESP_OK=0;
esp_app_desc_t installed{};
int result=ESP_OK;
int esp_ota_get_partition_description(const esp_partition_t*,esp_app_desc_t* out) {
    *out=installed;return result;
}
''' + guard + r'''
int main(){
 esp_partition_t slot;
 std::strcpy(installed.project_name,"tab5_updater");
 std::strcpy(installed.version,"1.1.0");assert(!MdAppPresent(&slot));
 std::strcpy(installed.version,"1.2.0");assert(MdAppPresent(&slot));
 std::strcpy(installed.version,"2.0.0");assert(MdAppPresent(&slot));
 assert(!MdAppPresent(nullptr));
 result=-1;assert(!MdAppPresent(&slot));result=ESP_OK;
 std::strcpy(installed.project_name,"other_app");assert(!MdAppPresent(&slot));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp=Path(directory)/"guard.cc";cpp.write_text(harness)
            binary=Path(directory)/"guard"
            subprocess.run(["c++","-std=c++17","-Wall","-Wextra","-Werror",
                            str(cpp),"-o",str(binary)],check=True)
            subprocess.run([str(binary)],check=True)


if __name__ == "__main__":
    unittest.main()
