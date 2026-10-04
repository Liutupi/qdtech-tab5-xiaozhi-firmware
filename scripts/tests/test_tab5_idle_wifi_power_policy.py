"""Exercise the real board and Application Wi-Fi power-save transitions on host."""

import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/qdtech/tab5"


class Tab5IdleWifiPowerPolicyTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_default_and_experimental_power_modes(self):
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text()
        option = kconfig.split("config QDTECH_TAB5_IDLE_BALANCED_WIFI_EXPERIMENT\n", 1)[1]
        option = option.split("\nconfig ", 1)[0]
        self.assertIn("depends on QDTECH_TAB5_NATIVE_UI && IDF_TARGET_ESP32P4", option)
        self.assertIn("default n", option)
        variants = json.loads((BOARD / "config.json").read_text())["builds"]
        native = next(item for item in variants if item["name"] == "qdtech-tab5-native")
        self.assertIn("CONFIG_QDTECH_TAB5_IDLE_BALANCED_WIFI_EXPERIMENT=y",
                      native["sdkconfig_append"])

        board_source = (BOARD / "m5stack_tab5.cc").read_text()
        board_begin = board_source.index("    void SetPowerSaveLevel(PowerSaveLevel level) override {",
                                         board_source.index("class QdtechTab5Board"))
        board_method = board_source[board_begin:
                                    board_source.index("\n    void StartNetwork()", board_begin)]
        generic_source = (ROOT / "main/boards/common/wifi_board.cc").read_text()
        generic_method = generic_source[
            generic_source.index("void WifiBoard::SetPowerSaveLevel(PowerSaveLevel level) {"):
            generic_source.index("\nstd::string WifiBoard::GetDeviceStatusJson()")
        ]
        app_source = (ROOT / "main/application.cc").read_text()
        app_method = app_source[
            app_source.index("void Application::SetExternalAudioActive(bool active) {"):
            app_source.index("\nvoid Application::PrepareExternalAudioPlayback()")
        ]
        harness = r'''
#include <atomic>
#include <functional>
#include <utility>
#include <vector>

#define ESP_LOGI(...) ((void)0)
#define TAG "test"

enum class PowerSaveLevel { LOW_POWER, BALANCED, PERFORMANCE };
enum class WifiPowerSaveLevel { LOW_POWER, BALANCED, PERFORMANCE };
#include "reply_wait_state.h"

class WifiManager {
public:
    WifiPowerSaveLevel last = WifiPowerSaveLevel::LOW_POWER;
    int calls = 0;
    static WifiManager& GetInstance() {
        static WifiManager manager;
        return manager;
    }
    void SetPowerSaveLevel(WifiPowerSaveLevel level) {
        last = level;
        ++calls;
    }
};

struct FakeCodec { void SetExternalPlaybackActive(bool) {} };
struct FakeBacklight {
    int value = 30;
    int brightness() const { return value; }
    void SetBrightness(int brightness, bool) { value = brightness; }
};
class Board {
public:
    static Board* current;
    static Board& GetInstance() { return *current; }
    virtual ~Board() = default;
    virtual void SetPowerSaveLevel(PowerSaveLevel level) = 0;
    FakeCodec* GetAudioCodec() { return &codec; }
    FakeBacklight* GetBacklight() { return &backlight; }
    FakeCodec codec;
    FakeBacklight backlight;
};
Board* Board::current = nullptr;

class WifiBoard : public Board {
public:
    void SetPowerSaveLevel(PowerSaveLevel level) override;
};
''' + generic_method + r'''

struct FakeAudioService {
    void SetExternalPlaybackActive(bool) {}
    void EnableVoiceProcessing(bool) {}
    void EnableWakeWordDetection(bool) {}
    void ResetDecoder() {}
};
class Application {
public:
    static Application& GetInstance() {
        static Application app;
        return app;
    }
    bool IsExternalAudioActive() const { return external_audio_active_.load(); }
    void SetExternalAudioActive(bool active);
    void Schedule(std::function<void()> callback) { pending.push_back(std::move(callback)); }
    int GetDeviceState() const { return kDeviceStateIdle; }
    void StartListeningAudio() {}
    std::atomic<bool> external_audio_active_{false};
    FakeAudioService audio_service_;
    ReplyWaitState reply_wait_;
    bool pending_listening_start_ = false;
    std::vector<std::function<void()>> pending;
};

class QdtechTab5Board : public WifiBoard {
public:
''' + board_method + r'''
};

class TracingWifiBoard : public WifiBoard {
public:
    std::vector<PowerSaveLevel> requested;
    void SetPowerSaveLevel(PowerSaveLevel level) override {
        requested.push_back(level);
        WifiBoard::SetPowerSaveLevel(level);
    }
};

''' + app_method + r'''

int main() {
    auto& app = Application::GetInstance();
    auto& wifi = WifiManager::GetInstance();
    QdtechTab5Board tab5;
    Board::current = &tab5;

    app.reply_wait_.Voice(true);
    app.reply_wait_.Voice(false);
    if (!app.reply_wait_.Recognized(0, kDeviceStateIdle)) return 8;
    app.SetExternalAudioActive(true);
    if (app.reply_wait_.Waiting(1, kDeviceStateIdle)) return 9;
    if (wifi.last != WifiPowerSaveLevel::BALANCED) return 1;
    tab5.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    if (wifi.last != WifiPowerSaveLevel::BALANCED) return 2;
    app.SetExternalAudioActive(false);
#if CONFIG_QDTECH_TAB5_IDLE_BALANCED_WIFI_EXPERIMENT
    if (wifi.last != WifiPowerSaveLevel::BALANCED) return 3;
#else
    if (wifi.last != WifiPowerSaveLevel::LOW_POWER) return 3;
#endif
    tab5.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    if (wifi.last != WifiPowerSaveLevel::PERFORMANCE) return 4;

    // The ordinary Wi-Fi board retains its existing three-level mapping.
    WifiBoard generic;
    Board::current = &generic;
    app.SetExternalAudioActive(true);
    if (wifi.last != WifiPowerSaveLevel::BALANCED) return 5;
    app.SetExternalAudioActive(false);
    if (wifi.last != WifiPowerSaveLevel::LOW_POWER) return 6;

    // Application must dispatch through Board for custom board policies.
    TracingWifiBoard custom;
    Board::current = &custom;
    app.SetExternalAudioActive(true);
    app.SetExternalAudioActive(false);
    if (custom.requested.size() != 2 ||
        custom.requested[0] != PowerSaveLevel::BALANCED ||
        custom.requested[1] != PowerSaveLevel::LOW_POWER) return 7;
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp_dir:
            source = Path(temp_dir) / "tab5_wifi_power_policy.cc"
            source.write_text(harness)
            for enabled in (0, 1):
                executable = Path(temp_dir) / f"tab5_wifi_power_policy_{enabled}"
                subprocess.run(
                    ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                     f"-DCONFIG_QDTECH_TAB5_IDLE_BALANCED_WIFI_EXPERIMENT={enabled}",
                     "-I", str(ROOT / "main"), str(source), "-o", str(executable)],
                    check=True, capture_output=True, text=True
                )
                subprocess.run([str(executable)], check=True, capture_output=True,
                               text=True)


if __name__ == "__main__":
    unittest.main()
