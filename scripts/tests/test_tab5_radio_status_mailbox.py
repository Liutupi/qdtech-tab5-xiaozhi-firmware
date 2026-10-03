"""Exercise the real radio UI callback and its fixed-size LVGL mailbox."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/qdtech/tab5"
BOARD_SOURCE = BOARD / "m5stack_tab5.cc"
DISPLAY_SOURCE = BOARD / "tab5_native_display.h"


def make_harness(source: str) -> str:
    method_start = source.index("    void OnMusicEndedNaturally() {")
    method_end = source.index("    // All source replacements", method_start)
    ended_method = source[method_start:method_end]
    callback_start = source.index("[this, native_display](const char* station")
    callback_end = source.index("});\n        native_radio_ready_", callback_start)
    callback = source[callback_start:callback_end + 1]
    return r'''
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "tab5_music_track_gate.h"
#include "tab5_radio_status_mailbox.h"

class Application {
public:
    static Application& GetInstance() {
        static Application app;
        return app;
    }
    void Schedule(std::function<void()>&& callback) {
        callbacks.push_back(std::move(callback));
    }
    void Drain() {
        auto ready = std::move(callbacks);
        callbacks.clear();
        for (auto& callback : ready) callback();
    }
    std::vector<std::function<void()>> callbacks;
};

struct FakeDisplay {
    tab5_radio_status::LatestMailbox mailbox;
    void PostRadioState(const char* station, const char* state, const char* meta) {
        mailbox.Post(station, state, meta);
    }
};

struct FakeRadioService {
    std::atomic<uint32_t> stream_generation{11};
    uint32_t GetStreamGeneration() const { return stream_generation.load(); }
};

class BoardHarness {
public:
    std::atomic<bool> music_continuous_{false};
    std::atomic<bool> music_continuous_session_{false};
    std::atomic<uint32_t> music_request_generation_{0};
    std::atomic<uint32_t> music_play_count_{0};
    std::mutex music_track_mutex_;
    tab5_music_track_gate::CurrentTrack music_track_;
    FakeRadioService radio_service_;
    FakeDisplay display;
    int next_requests = 0;
    void RequestNextSong(int delay_ms) {
        if (delay_ms != 800) __builtin_trap();
        ++next_requests;
    }
''' + ended_method + r'''
    void Emit(const char* station, const char* state, const char* meta) {
        auto* native_display = &display;
        auto callback = ''' + callback + r''';
        callback(station, state, meta);
    }
};

int main() {
    using tab5_radio_status::Snapshot;
    static_assert(sizeof(Snapshot) == 932, "radio status must stay bounded");
    BoardHarness board;
    auto& app = Application::GetInstance();
    Snapshot status;

    // Status strings from the producer are temporary; the UI receives its own copy.
    char station[] = "甲的歌曲";
    char state[] = "Playing";
    char meta[] = "MP3 320 kbps";
    board.Emit(station, state, meta);
    station[0] = 'x'; state[0] = 'x'; meta[0] = 'x';
    if (!board.display.mailbox.Take(status) ||
        std::strcmp(status.station.data(), "甲的歌曲") ||
        std::strcmp(status.state.data(), "Playing") ||
        std::strcmp(status.meta.data(), "MP3 320 kbps")) return 1;
    if (board.display.mailbox.Take(status)) return 2;

    // The UI may only take the latest of several decoder statuses. Preserve
    // each HTTP open across that coalescing so the lyric clock can rewind.
    tab5_radio_status::LatestMailbox opens;
    opens.Post("甲的歌曲", "Connecting", "opening");
    opens.Post("甲的歌曲", "Buffering", "filling");
    opens.Post("甲的歌曲", "Playing", "frame 1");
    if (!opens.Take(status) || std::strcmp(status.state.data(), "Playing") ||
        status.stream_open_sequence != 1) return 13;
    opens.Post("甲的歌曲", "Paused", "voice");
    opens.Post("甲的歌曲", "Playing", "resume");
    if (!opens.Take(status) || status.stream_open_sequence != 1) return 14;
    opens.Post("甲的歌曲", "Connecting", "retry");
    opens.Post("甲的歌曲", "Buffering", "retry");
    opens.Post("甲的歌曲", "Playing", "frame 1 again");
    if (!opens.Take(status) || status.stream_open_sequence != 2) return 15;

    // A burst has one latest UI snapshot, but the natural-end control event is
    // independent and cannot be overwritten by the following Connecting state.
    board.music_continuous_ = true;
    board.music_continuous_session_ = true;
    board.music_request_generation_ = 7;
    board.music_play_count_ = 3;
    board.music_track_.Begin(7, "甲的歌曲", "");
    board.Emit("甲的歌曲", "Playing", "MP3");
    board.Emit("甲的歌曲", "Stopped", "MP3 0 kbps Music ended");
    board.Emit("甲的歌曲", "Stopped", "MP3 0 kbps Music ended");
    board.Emit("下一首", "Connecting", "MP3");
    if (app.callbacks.size() != 2 || !board.display.mailbox.Take(status) ||
        std::strcmp(status.station.data(), "下一首") ||
        std::strcmp(status.state.data(), "Connecting")) return 3;
    app.Drain();
    if (board.next_requests != 1) return 4;

    // Stop/Next invalidates the scheduled natural-end request before it runs.
    board.music_continuous_ = true;
    board.music_continuous_session_ = true;
    board.Emit("下一首", "Stopped", "Music ended");
    ++board.music_request_generation_;
    board.music_continuous_session_ = false;
    app.Drain();
    if (board.next_requests != 1) return 5;

    // A newly arrived play_url also invalidates an earlier end event, even
    // during the short interval before PlayMusicRequest increments generation.
    board.music_continuous_ = true;
    board.music_continuous_session_ = true;
    board.Emit("下一首", "Stopped", "Music ended");
    ++board.music_play_count_;
    app.Drain();
    if (board.next_requests != 1) return 6;

    // A delayed old-song end can arrive after the board reserved a new
    // generation but before PlayUrlFromTool submits it to RadioService.
    // The old end's stream token must not advance the new continuous song.
    board.music_request_generation_ = 14;
    board.music_play_count_ = 7;
    board.music_continuous_ = true;
    board.music_continuous_session_ = true;
    board.radio_service_.stream_generation = 20;
    board.Emit("旧歌", "Stopped", "Music ended");
    board.music_track_.Begin(14, "新歌", "");
    board.radio_service_.stream_generation = 21;
    app.Drain();
    if (board.next_requests != 1) return 11;

    // If the queued event runs before new StartMusicNow, the board track gate
    // itself also rejects the reserved-but-not-started generation.
    board.music_request_generation_ = 15;
    board.music_play_count_ = 8;
    board.music_continuous_ = true;
    board.music_continuous_session_ = true;
    board.Emit("新歌", "Stopped", "Music ended");
    app.Drain();
    if (board.next_requests != 1) return 12;

    // Truncation must preserve a complete UTF-8 prefix of an oversized title.
    std::string long_title(766, 'a');
    long_title += "你";
    board.display.mailbox.Post(long_title.c_str(), "Playing", "");
    if (!board.display.mailbox.Take(status) ||
        std::strlen(status.station.data()) != 766 ||
        status.station[765] != 'a') return 7;

    // Latest-wins storage cannot grow with long-running radio status updates.
    for (int i = 0; i < 1000; ++i)
        board.Emit("频道", i == 999 ? "Stopped" : "Playing", "test");
    if (!board.display.mailbox.Take(status) ||
        std::strcmp(status.state.data(), "Stopped") ||
        board.display.mailbox.Take(status)) return 8;

    // The consumer owns the taken snapshot after unlocking. Concurrent posts
    // may replace the pending slot but cannot tear the displayed triple.
    tab5_radio_status::LatestMailbox concurrent;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (int i = 0; i < 5000; ++i) {
            char value[24];
            std::snprintf(value, sizeof(value), "%d", i);
            concurrent.Post(value, value, value);
        }
        done = true;
    });
    int consumed = 0;
    bool consistent = true;
    for (;;) {
        if (concurrent.Take(status)) {
            if (std::strcmp(status.station.data(), status.state.data()) ||
                std::strcmp(status.station.data(), status.meta.data())) consistent = false;
            ++consumed;
        } else if (done.load()) {
            break;
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();
    if (!consistent) return 9;
    if (!consumed) return 10;
}
'''


class Tab5RadioStatusMailboxTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ host compiler required")
    def test_latest_snapshot_ownership_and_natural_end_generation(self):
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "radio_status_mailbox.cc"
            binary = Path(directory) / "radio_status_mailbox"
            cpp.write_text(make_harness(BOARD_SOURCE.read_text()))
            build = subprocess.run(
                ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                 "-I", str(BOARD), str(cpp), "-o", str(binary)],
                capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_display_tick_consumes_snapshot_inside_lvgl_context(self):
        source = DISPLAY_SOURCE.read_text()
        tick_start = source.index("    void Tick() {")
        tick = source[tick_start:source.index("\npublic:", tick_start)]
        self.assertIn("tab5_radio_status::Snapshot radio_status_current_;", source)
        self.assertIn("radio_status_mailbox_.Take(radio_status_current_)", tick)
        self.assertNotIn("tab5_radio_status::Snapshot radio_status;", tick)
        self.assertIn("UpdateMusicPlaybackStateLocked", tick)
        self.assertIn("apps_->SetRadioState", tick)
        self.assertNotIn("DisplayLockGuard lock(this)", tick)
        self.assertIn("->Tick();", source[source.index("animation_timer_ = lv_timer_create("):])
        board_source = BOARD_SOURCE.read_text()
        callback = board_source[board_source.index("[this, native_display](const char* station"):]
        self.assertIn("native_display->PostRadioState", callback.split("});", 1)[0])
        self.assertNotIn("native_display->SetRadioState", callback.split("});", 1)[0])


if __name__ == "__main__":
    unittest.main()
