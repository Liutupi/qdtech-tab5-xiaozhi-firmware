#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "tab5_home_model.h"

// 米家中控 client: reads Home Assistant devices and combined scenes from the NAS relay
// (same relay and public URL as the Muse inbox) and sends control requests. Through the
// public tunnel the relay requires a home key; the Tab5 obtains it once by pairing and keeps
// it in NVS. All network work runs on one small PSRAM-stack task.
namespace tab5_home {

struct Status {
    bool configured = false;  // relay URL known
    bool paired = false;      // home key present
    bool ok = false;          // last catalog fetch succeeded
    std::string message;      // short Chinese status for the page / voice replies
    std::shared_ptr<const Catalog> catalog;
    uint32_t version = 0;     // bumps on every catalog or status change
};

class Hub {
public:
    using Listener = std::function<void(const Status&)>;
    using Done = std::function<void(bool ok, const std::string& message)>;

    static Hub& GetInstance();
    // Main task (reads NVS). listener runs on the hub task.
    void Start(Listener listener);
    void SetPageVisible(bool visible);  // poll every few seconds while the page is open
    void RequestRefresh();
    Status Current();
    // Queue a control (action: on/off/toggle/brightness/temperature/hvac_mode) or a scene
    // run (action: on/off). done runs on the hub task. Returns false when the queue is full.
    bool Control(const std::string& entity_id, const std::string& action, const std::string& value,
                 Done done = {});
    bool RunScene(const std::string& scene_id, bool on, Done done = {});

private:
    struct Job {
        std::string path;  // control or scene
        std::string body;
        Done done;
    };
    Hub() = default;
    static void TaskEntry(void* arg);
    void Run();
    bool Pair(const std::string& base);
    bool Fetch(const std::string& base);
    void Execute(const std::string& base, Job job);
    void Publish(bool ok, const std::string& message, std::shared_ptr<const Catalog> catalog);
    std::string Base();

    std::mutex mutex_;
    Status status_;
    Listener listener_;
    std::string key_;
    std::vector<Job> jobs_;
    bool refresh_ = false;
    bool visible_ = false;
    TaskHandle_t task_ = nullptr;
};

}  // namespace tab5_home
