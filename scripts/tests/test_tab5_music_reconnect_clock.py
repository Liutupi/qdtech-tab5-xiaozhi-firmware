"""Run the real Tab5 lyric clock methods through a Range resume using the real audio-output clock."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/qdtech/tab5"
DISPLAY = BOARD / "tab5_native_display.h"


def method(source: str, start: str, end: str) -> str:
    return source[source.index(start):source.index(end, source.index(start))]


def harness(source: str) -> str:
    refresh = method(source, "    void RefreshMusicLyrics() {", "    // Called only while LVGL")
    playback = method(source, "    void UpdateMusicPlaybackStateLocked(", "#ifdef CONFIG_QDTECH_TAB5_PPA_FLUSH_EXPERIMENT")
    return r'''
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include "tab5_playback_clock.h"
#include <string>
#include "tab5_lrc.h"
#include "tab5_radio_status_mailbox.h"

int64_t now_us = 1000000;
int64_t esp_timer_get_time() { return now_us; }
struct FakeLabel { std::string text; };
void lv_label_set_text(FakeLabel* label, const char* text) { label->text = text; }
struct FakeApps {
    std::string current;
    void SetMusicLyricsWindow(const char*, const char*, const char*, const char* line,
                              const char*) { current = line; }
    void ClearMusicLyrics() { current.clear(); }
};

class DisplayHarness {
public:
    std::string music_title_ = "同一首歌";
    std::string music_artist_ = "歌手";
    tab5_lrc::Document music_lyrics_;
    int music_line_index_ = -2;
    std::string music_source_url_ = "https://relay/audio.mp3";
    std::function<tab5_playback::Position(std::string_view)> music_position_;
    bool music_playing_ = false;
    bool music_active_ = true;
    bool music_seen_station_ = false;
    uint64_t next_idle_reaction_ms_ = 0;
    FakeLabel label;
    FakeLabel* message_label_ = &label;
    FakeApps app;
    FakeApps* apps_ = &app;
''' + refresh + playback + r'''
};

int main() {
    DisplayHarness display;
    tab5_playback::Clock clock;
    clock.Begin(display.music_source_url_, 7);
    display.music_position_ = [&](std::string_view url) { return clock.Read(url, 7); };
    if (!tab5_lrc::Parse("[00:00.00]开头\n[00:08.00]后段\n", display.music_lyrics_))
        return 1;
    tab5_radio_status::LatestMailbox mailbox;
    tab5_radio_status::Snapshot status;
    auto consume = [&]() {
        if (!mailbox.Take(status)) return false;
        display.UpdateMusicPlaybackStateLocked(status.station.data(), status.state.data());
        display.RefreshMusicLyrics();
        return true;
    };

    // The first HTTP open is followed by several statuses before the UI tick.
    mailbox.Post("同一首歌 - 歌手", "Connecting", "open");
    mailbox.Post("同一首歌 - 歌手", "Buffering", "fill");
    mailbox.Post("同一首歌 - 歌手", "Playing", "first frame");
    if (!consume() || !display.music_playing_ || display.app.current != "开头") return 2;
    // Waiting and UI delays do not count as audible playback.
    now_us += 9000000;
    display.RefreshMusicLyrics();
    if (display.app.current != "开头") return 3;
    clock.Advance(9*24000, 24000, 7);
    display.RefreshMusicLyrics();
    if (display.app.current != "后段") return 10;

    // A normal pause/resume keeps the position when no new HTTP stream opened.
    mailbox.Post("同一首歌 - 歌手", "Paused", "voice");
    if (!consume() || display.music_playing_) return 4;
    now_us += 5000000;
    mailbox.Post("同一首歌 - 歌手", "Playing", "resume");
    if (!consume() || display.app.current != "后段") return 5;

    // Range creates another HTTP connection while keeping the saved output time.
    mailbox.Post("同一首歌 - 歌手", "Reconnecting", "read failed");
    if (!consume()) return 6;
    clock.Begin(display.music_source_url_, 7, 9000);
    mailbox.Post("同一首歌 - 歌手", "Connecting", "range open");
    mailbox.Post("同一首歌 - 歌手", "Buffering", "preroll");
    mailbox.Post("同一首歌 - 歌手", "Playing", "next frame");
    if (!consume() || !display.music_playing_ || display.app.current != "后段") return 7;
    clock.Advance(24000,24000,7);
    now_us += 20000000;
    display.RefreshMusicLyrics();
    if (display.app.current != "后段") return 8;
    // A genuinely new track starts at zero; an old decoder cannot advance it.
    clock.Begin(display.music_source_url_, 8);
    clock.Advance(24000,24000,7);
    display.RefreshMusicLyrics();
    if (display.app.current != "后段") return 9; // mismatched generation ignored
    display.music_position_ = [&](std::string_view url) { return clock.Read(url, 8); };
    display.RefreshMusicLyrics();
    if (display.app.current != "开头") return 11;
    return 0;
}
'''


class Tab5MusicReconnectClockTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ host compiler required")
    def test_range_resume_keeps_audio_position_even_when_status_is_coalesced(self):
        source = DISPLAY.read_text()
        self.assertIn("music_position_(music_source_url_)", source)
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "music_reconnect_clock.cc"
            binary = Path(directory) / "music_reconnect_clock"
            cpp.write_text(harness(source))
            build = subprocess.run(
                ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(BOARD),
                 str(cpp), str(BOARD / "tab5_lrc.cc"), "-o", str(binary)],
                capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
