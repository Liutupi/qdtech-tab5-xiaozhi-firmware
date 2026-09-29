#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Online firmware upgrade for the Tab5 (Settings -> 固件升级).
//
// Flow: query the newest GitHub release, download `qdtech-tab5-<tag>-app.bin` to the SD card,
// verify its SHA-256 against the release's SHA256SUMS.txt, then reboot into the small updater
// stored in the `ota_0` partition. The updater rewrites the `factory` partition from the SD
// file, verifies it again and boots the new firmware. NVS (Wi-Fi, settings) is left untouched.
class Tab5Ota {
public:
    struct Status {
        std::string text;    // one line shown on the Settings card
        std::string button;  // caption for the action button
        int progress = -1;   // 0..100, or -1 to hide the bar
        bool busy = false;   // true while checking / downloading
    };
    using StatusCallback = std::function<void(const Status&)>;

    static Tab5Ota& GetInstance();

    // The callback may be invoked from a worker task; the caller takes the LVGL lock.
    void SetStatusCallback(StatusCallback callback);
    Status GetStatus();
    std::string CurrentVersion() const;

    // Check for a newer release; when one is already known, download and install it.
    void HandleButton();

private:
    struct ReleaseInfo {
        std::string tag;
        std::string version;
        std::string asset_name;
        std::string asset_url;
        std::string sha256;
        size_t asset_size = 0;
    };
    enum class Action { Check, Install };

    std::mutex mutex_;
    StatusCallback callback_;
    Status status_;
    ReleaseInfo release_;
    bool update_ready_ = false;  // `release_` holds a newer, installable release
    std::atomic<bool> busy_{false};

    static void TaskEntry(void* arg);
    void Run(Action action);
    void Check();
    void Install();
    bool FetchLatestRelease(ReleaseInfo* release);
    void SetStatus(const std::string& text, const std::string& button, int progress, bool busy);
    bool Download(const ReleaseInfo& release, const char* path);
};
