"""Run the real native radio status renderer against playback-intent transitions."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/qdtech/tab5"


class RadioPlayIntentUiTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_button_follows_intent_while_wave_follows_playing(self):
        ui_source = (BOARD / "tab5_native_apps.cc").read_text()
        renderer = ui_source[
            ui_source.index("void Tab5NativeApps::SetRadioState(") :
            ui_source.index("\n}", ui_source.index("void Tab5NativeApps::SetRadioState(")) + 2
        ]
        radio_header = (BOARD / "radio_service.h").read_text()
        getter = next(
            line.strip() for line in radio_header.splitlines()
            if line.strip().startswith("bool IsPlayRequested() const")
        )
        board_source = (BOARD / "m5stack_tab5.cc").read_text()
        binding = next(
            line.strip() for line in board_source.splitlines()
            if line.strip().startswith("actions.radio_play_requested =")
        )
        harness = r'''
#include <atomic>
#include <cstring>
#include <functional>
#include <string>

struct lv_obj_t { std::string text; };

class RadioService {
public:
    std::atomic<bool> play_requested_{false};
    ''' + getter + r'''
};

class Tab5NativeApps {
public:
    struct Actions { std::function<bool()> radio_play_requested; } actions_;
    lv_obj_t station_label, state_label, meta_label, play_label;
    lv_obj_t* radio_station_ = &station_label;
    lv_obj_t* radio_state_ = &state_label;
    lv_obj_t* radio_meta_ = &meta_label;
    lv_obj_t* radio_play_label_ = &play_label;
    // Muse 电台 page is created lazily; these harnesses never open it.
    struct PodcastStub {
        void Tick() {}
        void SetPlayback(bool, const char*) {}
        template <typename T> void SetEpisodes(const T&) {}
    };
    PodcastStub* podcast_page_ = nullptr;
    std::string current_station_name_;
    std::string radio_station_name_;
    bool radio_playing_ = false;
    int station_refreshes = 0;
    static void SetLabelTextIfChanged(lv_obj_t* label, const char* text) {
        if (label && label->text != text) label->text = text;
    }
    void RefreshStations() { ++station_refreshes; }
    void SetRadioState(const char* station, const char* state, const char* meta);
};

class BoardHarness {
public:
    RadioService radio_service_;
    Tab5NativeApps::Actions MakeActions() {
        Tab5NativeApps::Actions actions;
        ''' + binding + r'''
        return actions;
    }
};

''' + renderer + r'''
int main() {
    BoardHarness board;
    Tab5NativeApps ui;
    ui.actions_ = board.MakeActions();
    auto check = [&](bool requested, const char* station, const char* state,
                     const char* expected_button, bool expected_wave,
                     const char* expected_status) {
        board.radio_service_.play_requested_.store(requested);
        ui.SetRadioState(station, state, "metadata");
        return ui.play_label.text == expected_button &&
               ui.radio_playing_ == expected_wave &&
               ui.state_label.text == expected_status &&
               ui.station_label.text == station;
    };

    if (!check(false, "电台甲", "Ready", "播放", false, "待播放")) return 1;
    if (!check(true, "电台甲", "Connecting", "暂停", false, "连接中")) return 2;
    if (!check(true, "电台甲", "Playing", "暂停", true, "播放中")) return 3;
    // Wi-Fi loss keeps the intent so reconnect can resume; the wave is idle.
    if (!check(true, "电台甲", "Waiting WiFi", "暂停", false, "等待网络")) return 4;
    if (!check(true, "电台甲", "Reconnecting", "暂停", false, "连接中")) return 5;
    if (!check(true, "电台甲", "Buffering", "暂停", false, "缓冲中")) return 6;
    if (!check(true, "电台甲", "Playing", "暂停", true, "播放中")) return 7;
    // Pressing Pause, normal Stop and a replacement song update intent.
    if (!check(false, "电台甲", "Paused", "播放", false, "已暂停")) return 8;
    if (!check(false, "电台甲", "Stopped", "播放", false, "已停止")) return 9;
    if (!check(true, "歌曲乙", "Connecting", "暂停", false, "连接中")) return 10;
    if (!check(true, "歌曲乙", "Playing", "暂停", true, "播放中")) return 11;
    if (!check(false, "歌曲乙", "Stopped", "播放", false, "已停止")) return 12;
    if (ui.station_refreshes != 2) return 13;
    // Before RadioService is available, status rendering retains its fallback.
    ui.actions_.radio_play_requested = {};
    ui.SetRadioState("歌曲乙", "Playing", "metadata");
    if (ui.play_label.text != "暂停") return 14;
    ui.SetRadioState("歌曲乙", "Waiting WiFi", "metadata");
    if (ui.play_label.text != "播放") return 15;
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp_dir:
            source = Path(temp_dir) / "radio_play_intent_ui.cc"
            executable = Path(temp_dir) / "radio_play_intent_ui"
            source.write_text(harness)
            subprocess.run(
                ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(source),
                 "-o", str(executable)], check=True, capture_output=True, text=True
            )
            subprocess.run([str(executable)], check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
