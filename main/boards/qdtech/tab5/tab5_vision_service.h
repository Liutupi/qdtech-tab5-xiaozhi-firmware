#pragma once

#include <atomic>
#include <cstdint>

class EspVideo;
class QdtechTab5Display;

class Tab5VisionService {
    EspVideo* camera_ = nullptr;
    QdtechTab5Display* display_ = nullptr;
    std::atomic<bool> test_requested_{false};
    std::atomic<bool> vision_ready_{false};
    std::atomic<bool> interaction_requested_{false};
    std::atomic<bool> greeting_owed_{false};
    std::atomic<bool> greeting_queued_{false};
    std::atomic<int64_t> last_greeting_us_{0};
    bool started_ = false;

    static void TaskEntry(void* arg);
    void Run();
    void TryGreeting(int64_t detected_at_us);

public:
    void Start(EspVideo* camera, QdtechTab5Display* display);
    void RequestPresenceTest();
    void NotifyInteraction() { interaction_requested_.store(true); }
};
