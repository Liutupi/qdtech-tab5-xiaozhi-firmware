"""Compile the Tab5 MCP lyric gate against the actual board methods."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/qdtech/tab5"
SOURCE = BOARD / "m5stack_tab5.cc"


def between(source: str, start: str, end: str) -> str:
    return source[source.index(start):source.index(end, source.index(start))]


def make_harness(source: str) -> str:
    replacement = between(source, "    template <typename Action>\n    void ReplaceMusicSource(",
                          "    void RequestNextSong(")
    methods = between(source, "    bool ApplyLegacyMusicLyricLine(",
                      "    // Lyrics are looked up while the song")
    return r'''
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "tab5_music_track_gate.h"

struct QdtechTab5Display {
    std::string title, artist, line, lyrics;
    bool active = false;
    std::mutex wait_mutex;
    std::condition_variable wait_cv;
    bool block_connect = false, connect_entered = false, resume_connect = false;
    void BeginMusicTrack(const char* next_title, const char* next_artist) {
        title = next_title; artist = next_artist; active = true;
        line.clear(); lyrics.clear();
    }
    void SetMusicInfo(const char* next_title, const char* next_artist, const char* next_line) {
        if (block_connect && std::string(next_line) == "正在连接音源…") {
            std::unique_lock<std::mutex> lock(wait_mutex);
            connect_entered = true;
            wait_cv.notify_all();
            wait_cv.wait(lock, [this] { return resume_connect; });
        }
        if (next_title && *next_title) title = next_title;
        if (next_artist && *next_artist) artist = next_artist;
        line = next_line;
    }
    bool SetMusicLyrics(const char* next_lyrics, const char* expected_title) {
        if (!active || title != expected_title) return false;
        lyrics = next_lyrics;
        return true;
    }
    void StopMusicTrack() { active = false; lyrics.clear(); }
};

struct FakeRadioService {
    bool fail = false;
    std::vector<std::string> commands;
    std::string PlayUrlFromTool(const std::string&, const std::string&,
                               const std::string&) {
        commands.push_back("play");
        return fail ? "Music URL was NOT started: failure" : "Music URL started";
    }
    void Stop() { commands.push_back("stop"); }
};

class BoardHarness {
public:
    QdtechTab5Display screen;
    QdtechTab5Display* display_ = &screen;
    FakeRadioService radio_service_;
    std::atomic<uint32_t> music_request_generation_{0};
    std::mutex music_track_mutex_;
    tab5_music_track_gate::CurrentTrack music_track_;
''' + replacement + methods + r'''
};

int main() {
    BoardHarness board;
    auto& screen = board.screen;
    const uint32_t first = ++board.music_request_generation_;
    {
        std::string title = "同名歌";
        std::string artist = "甲";
        if (board.StartMusicNow(first, title, artist, "https://song/1", "") !=
            "Music URL started") return 1;
        title.clear(); artist.clear();  // The gate owns the identity strings.
    }
    if (!board.ApplyLegacyMusicLyricLine("同名歌", "甲", "第一句")) return 2;
    if (screen.title != "同名歌" || screen.artist != "甲" || screen.line != "第一句")
        return 3;
    if (board.ApplyLegacyMusicLyricLine("", "甲", "unidentified")) return 4;
    if (board.ApplyLegacyMusicLyricLine("另一首", "甲", "stale")) return 5;

    // A later play_url with the same title but a different singer must reject
    // the old singer and title-only packets while accepting the new identity.
    const uint32_t second = ++board.music_request_generation_;
    if (board.StartMusicNow(second, "同名歌", "乙", "https://song/2", "") !=
        "Music URL started") return 6;
    if (board.ApplyLegacyMusicLyricLine("同名歌", "甲", "late from first")) return 7;
    if (board.ApplyLegacyMusicLyricLine("同名歌", "", "ambiguous")) return 8;
    if (board.ApplyMusicLyricsFromTool("同名歌", "甲", "[00:01]stale")) return 9;
    if (board.ApplyMusicLyricsForTrack(first, "同名歌", "甲", "[00:01]late lookup"))
        return 23;
    if (board.ApplyMusicLyricsFromTool("", "", "[00:01]unidentified")) return 10;
    if (screen.title != "同名歌" || screen.artist != "乙" ||
        screen.line != "正在连接音源…" || !screen.lyrics.empty()) return 11;
    if (!board.ApplyLegacyMusicLyricLine("同名歌", "乙", "第二句")) return 12;
    if (!board.ApplyMusicLyricsFromTool("同名歌", "乙", "[00:01]第二句")) return 13;
    if (screen.title != "同名歌" || screen.artist != "乙" ||
        screen.line != "第二句" || screen.lyrics != "[00:01]第二句") return 14;

    // Title-only legacy callers still work for a distinct new title.
    const uint32_t third = ++board.music_request_generation_;
    if (board.StartMusicNow(third, "新歌", "丙", "https://song/3", "") !=
        "Music URL started") return 15;
    if (!board.ApplyLegacyMusicLyricLine("新歌", "", "新句")) return 16;
    if (!board.ApplyMusicLyricsFromTool("新歌", "", "[00:01]新句")) return 17;

    // Stop/Next invalidates the gate before a replacement has started.
    const uint32_t fourth = ++board.music_request_generation_;
    if (board.ApplyLegacyMusicLyricLine("新歌", "丙", "late after stop")) return 18;
    if (board.ApplyMusicLyricsFromTool("新歌", "丙", "[00:01]late")) return 19;
    if (board.StartMusicNow(third, "stale", "", "https://song/old", "") !=
        "Music URL was NOT started: superseded by a newer request.") return 20;

    board.radio_service_.fail = true;
    if (board.StartMusicNow(fourth, "失败", "", "https://song/fail", "") !=
        "Music URL was NOT started: failure") return 21;
    if (screen.active || board.ApplyLegacyMusicLyricLine("失败", "", "late")) return 22;

    // Deterministic Stop-during-LVGL-wait interleaving: Stop must wait for the
    // old play submission and then win. Before the shared lock, this was
    // Stop -> Play and the old song resumed after the user's Stop tap.
    BoardHarness racing;
    auto& display = racing.screen;
    display.block_connect = true;
    const uint32_t racing_generation = ++racing.music_request_generation_;
    std::string start_result;
    std::thread starter([&] {
        start_result = racing.StartMusicNow(racing_generation, "竞态歌", "甲",
                                           "https://song/race", "[00:01]歌词");
    });
    {
        std::unique_lock<std::mutex> lock(display.wait_mutex);
        display.wait_cv.wait(lock, [&] { return display.connect_entered; });
    }
    std::thread stopper([&] {
        racing.ReplaceMusicSource([&] {
            racing.radio_service_.Stop();
            display.StopMusicTrack();
            display.SetMusicInfo("已停止", "", "点歌或收听电台");
        });
    });
    {
        std::lock_guard<std::mutex> lock(display.wait_mutex);
        display.resume_connect = true;
    }
    display.wait_cv.notify_all();
    starter.join();
    stopper.join();
    if (start_result != "Music URL started" ||
        racing.radio_service_.commands != std::vector<std::string>{"play", "stop"})
        return 24;
    if (display.active || display.title != "已停止" ||
        racing.music_request_generation_ != racing_generation + 1) return 25;
}
'''


class Tab5MusicTrackGateTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ host compiler required")
    def test_real_board_methods_reject_stale_lyrics(self):
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "music_track_gate.cc"
            binary = Path(directory) / "music_track_gate"
            cpp.write_text(make_harness(SOURCE.read_text()))
            build = subprocess.run(
                ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                 "-I", str(BOARD), str(cpp), "-o", str(binary)],
                capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_mcp_handlers_use_the_gate(self):
        source = SOURCE.read_text()
        native_tools = between(source, 'mcp.AddTool(\n            "self.music.set_lyrics",',
                               'mcp.AddTool("self.music.stop"')
        self.assertIn("ApplyMusicLyricsFromTool(title, artist, lyrics)", native_tools)
        self.assertEqual(native_tools.count("return ApplyLegacyMusicLyricLine("), 2)
        self.assertNotIn("->SetMusicInfo(", native_tools)
        self.assertNotIn("->SetMusicLyrics(", native_tools)
        lookup = between(source, "    static void MusicLookupTask(",
                         "    std::string PlayMusicRequest(")
        self.assertIn("ApplyMusicLyricsForTrack(generation, title, artist, lyrics)", lookup)
        self.assertNotIn("->SetMusicLyrics(", lookup)

    def test_native_source_replacements_share_the_track_lock(self):
        source = SOURCE.read_text()
        # One increment is inside ReplaceMusicSource; the other reserves a
        # play_url generation while holding the same mutex.
        self.assertEqual(source.count("++music_request_generation_"), 2)
        reservation = between(source, "    std::string PlayMusicRequest(",
                              "    // ---- LAN UDP control")
        self.assertIn("std::lock_guard<std::mutex> guard(music_track_mutex_);", reservation)
        self.assertIn("NoteMusicPlayRequest(continuous);", reservation)
        self.assertGreaterEqual(source.count("ReplaceMusicSource([this"), 14)


if __name__ == "__main__":
    unittest.main()
