"""米家中控 catalog: relay TSV parsing, pairing key, URL mapping and voice matching."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

BOARD = Path(__file__).resolve().parents[2] / "main/boards/qdtech/tab5"

SMOKE = r'''
#include <cassert>
#include <cstdio>
#include "tab5_home_model.h"
using namespace tab5_home;
int main() {
    const char* tsv =
        "V\t1\n"
        "D\tscene.cinema_on\t家庭影院开\tscene\t2025-12-09T16:49:28\t客厅\t-\t-\t-\n"
        "D\tswitch.bedroom_pc\t卧室电脑开机\tswitch\toff\t卧室\t-\t-\t-\n"
        "D\tlight.bulb\t灯泡\tlight\ton\t书房\t200\t-\t-\n"
        "D\tlight.bar\t显示器挂灯\tlight\tunavailable\t卧室\t-\t-\t-\n"
        "D\tclimate.bedroom\t空调\tclimate\tcool\t卧室\t-\t26\t27.5\n"
        "D\tclimate.kids\t空调\tclimate\toff\t小孩房间\t-\t24\t-\n"
        "D\tmedia_player.tv\t客厅的小米电视\tmedia_player\toff\t客厅\t-\t-\t-\n"
        "D\tbroken\tno dot\tswitch\ton\t\t-\t-\t-\n"
        "S\tbedroom_pc\t卧室电脑\t卧室\t1\t1\n"
        "S\tcinema\t家庭影院\t客厅\t1\t1\n"
        "X\tfuture\trecord\n";
    Catalog c;
    assert(ParseCatalog(tsv, &c));
    assert(c.devices.size() == 7 && c.scenes.size() == 2);
    assert(c.devices[2].brightness == 200 && c.devices[4].target == 26.0f && c.devices[4].current == 27.5f);
    assert(c.devices[5].current == -1000.0f);
    assert(!c.devices[0].on() && c.devices[0].kind() == Kind::kScene);
    assert(!c.devices[1].on() && c.devices[2].on() && !c.devices[3].available() && !c.devices[3].on());
    assert(c.devices[4].on() && !c.devices[5].on());
    assert(c.scenes[1].has_on && c.scenes[1].has_off && c.scenes[1].room == "客厅");
    Catalog same;
    assert(ParseCatalog(tsv, &same) && SameCatalog(c, same));
    same.devices[1].state = "on";
    assert(!SameCatalog(c, same));
    Catalog bad;
    assert(!ParseCatalog("{\"ok\":true}", &bad) && bad.devices.empty());
    assert(!ParseCatalog("", &bad));

    // Scenes: longest name wins.
    assert(MatchScene(c, "打开家庭影院") == 1);
    assert(MatchScene(c, "把卧室电脑关了") == 0);
    assert(MatchScene(c, "开灯") == -1);
    // Devices: label, room tie-break, then room + kind word.
    assert(MatchDevice(c, "打开卧室电脑开机") == 1);
    assert(MatchDevice(c, "把小孩房间的空调关掉") == 5);
    assert(MatchDevice(c, "卧室空调调到26度") == 4);
    assert(MatchDevice(c, "打开书房的灯") == 2);
    assert(MatchDevice(c, "关闭客厅的小米电视") == 6);
    assert(MatchDevice(c, "打开厨房的灯") == -1);

    assert(ParsePairKey("K\tabcdefghijklmnopqrstuvwxyz012345\n") == "abcdefghijklmnopqrstuvwxyz012345");
    assert(ParsePairKey("K\tshort\n").empty() && ParsePairKey("{\"key\":\"x\"}").empty());
    assert(ParsePairKey("K\tabcdefghijklmnopqrstuvwxyz01234!\n").empty());

    assert(HomeBase("https://a-b.trycloudflare.com/inbox/TOKEN_1") == "https://a-b.trycloudflare.com/home/TOKEN_1");
    assert(HomeBase("https://x.example/mcp/tok?x=1") == "https://x.example/home/tok");
    assert(HomeBase("https://x.example/inbox/").empty() && HomeBase("http://192.168.3.200:8787").empty());
    std::puts("PASS home catalog, pairing key, URL mapping, voice matching");
}
'''


class HomeModelTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_home_model(self):
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "home.cc"
            cpp.write_text(SMOKE)
            binary = Path(directory) / "home"
            subprocess.run(["c++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(BOARD), str(cpp),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
