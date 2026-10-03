"""Run the real Tab5 lyric clock methods through a coalesced stream retry."""

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
    int64_t music_elapsed_ms_ = 0;
    int64_t music_resume_ms_ = 0;
    uint32_t music_stream_open_sequence_ = 0;
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
    if (!tab5_lrc::Parse("[00:00.00]开头\n[00:08.00]后段\n", display.music_lyrics_))
        return 1;
    tab5_radio_status::LatestMailbox mailbox;
    tab5_radio_status::Snapshot status;
    auto consume = [&]() {
        if (!mailbox.Take(status)) return false;
        display.UpdateMusicPlaybackStateLocked(status.station.data(), status.state.data(),
                                               status.stream_open_sequence);
        display.RefreshMusicLyrics();
        return true;
    };

    // The first HTTP open is followed by several statuses before the UI tick.
    mailbox.Post("同一首歌 - 歌手", "Connecting", "open");
    mailbox.Post("同一首歌 - 歌手", "Buffering", "fill");
    mailbox.Post("同一首歌 - 歌手", "Playing", "first frame");
    if (!consume() || !display.music_playing_ || display.app.current != "开头") return 2;
    now_us += 9000000;
    display.RefreshMusicLyrics();
    if (display.app.current != "后段") return 3;

    // A normal pause/resume keeps the position when no new HTTP stream opened.
    mailbox.Post("同一首歌 - 歌手", "Paused", "voice");
    if (!consume() || display.music_playing_) return 4;
    now_us += 5000000;
    mailbox.Post("同一首歌 - 歌手", "Playing", "resume");
    if (!consume() || display.app.current != "后段") return 5;

    // The reader fails; retry GET starts again from byte zero. Connecting and
    // Buffering may both be lost to latest-state coalescing before first frame.
    mailbox.Post("同一首歌 - 歌手", "Reconnecting", "read failed");
    if (!consume()) return 6;
    mailbox.Post("同一首歌 - 歌手", "Connecting", "retry open");
    mailbox.Post("同一首歌 - 歌手", "Buffering", "retry fill");
    mailbox.Post("同一首歌 - 歌手", "Playing", "retry first frame");
    if (!consume() || !display.music_playing_ || display.app.current != "开头" ||
        display.music_elapsed_ms_ != 0 || display.music_stream_open_sequence_ != 2) return 7;
    now_us += 9000000;
    display.RefreshMusicLyrics();
    if (display.app.current != "后段") return 8;

    // More status updates in the same stream must not rewind the song again.
    mailbox.Post("同一首歌 - 歌手", "Buffering", "temporary gap");
    mailbox.Post("同一首歌 - 歌手", "Playing", "same stream");
    if (!consume() || display.app.current != "后段" || display.music_elapsed_ms_ != 0) return 9;
    return 0;
}
'''


class Tab5MusicReconnectClockTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ host compiler required")
    def test_restart_rewinds_lyrics_even_when_connecting_is_coalesced(self):
        source = DISPLAY.read_text()
        self.assertIn("radio_status_current_.stream_open_sequence", source)
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
