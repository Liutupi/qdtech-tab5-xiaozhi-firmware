"""Muse radio episode parsing: track lines, intro text and emoji stripping."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

BOARD = Path(__file__).resolve().parents[2] / "main/boards/qdtech/tab5"

SMOKE = r'''
#include <cassert>
#include <cstdio>
#include "tab5_podcast_model.h"
using namespace tab5_podcast;
int main() {
    const std::string post =
        "🎙️ 刘主任，早上好！今天为你精选10首歌：\n\n"
        "1.《从军记》— 三官庙红白事之王\n"
        "2.《Outer Wilds》— Andrew Prahlow\n"
        "3. 《Fix You》 - Coldplay\n"
        "4、《稻香》——周杰伦\n"
        "10.《我是一棵秋天的树 (Live in summer)》— 苏打绿\n"
        "5)《无歌手》\n"
        "Song 6《不是列表》— x\n"
        "\n愿音乐陪你开启元气满满的一天 ☀️";
    auto t = ParseTracks(post);
    assert(t.size() == 6);
    assert(t[0].title == "从军记" && t[0].artist == "三官庙红白事之王");
    assert(t[2].title == "Fix You" && t[2].artist == "Coldplay");
    assert(t[3].title == "稻香" && t[3].artist == "周杰伦");
    assert(t[4].title == "我是一棵秋天的树 (Live in summer)" && t[4].artist == "苏打绿");
    assert(t[5].title == "无歌手" && t[5].artist.empty());
    assert(DisplayText(Intro(post)) == "刘主任，早上好！今天为你精选10首歌：");
    assert(DisplayText("愿音乐陪你开启元气满满的一天 ☀️") == "愿音乐陪你开启元气满满的一天");
    assert(DisplayText("A 🎵 B") == "A B");
    assert(IsRadioPost("每日电台", "x") && IsRadioPost("Muse", "10月03日·每日音乐电台"));
    assert(!IsRadioPost("Muse", "10月2日专业资讯"));
    std::string many;
    for (int i = 1; i <= 40; ++i) many += std::to_string(i % 100) + ".《s》— a\n";
    assert(ParseTracks(many).size() == kMaxTracks);
    std::puts("PASS track lines, intro, emoji stripping, radio-post detection, track cap");
}
'''


class Tab5PodcastModelTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_parsing(self):
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "podcast.cc"
            cpp.write_text(SMOKE)
            binary = Path(directory) / "podcast"
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(BOARD),
                            str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
