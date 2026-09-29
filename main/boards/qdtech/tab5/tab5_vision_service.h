#pragma once

#include <atomic>

class EspVideo;
class QdtechTab5Display;

class Tab5VisionService {
    EspVideo* camera_ = nullptr;
    QdtechTab5Display* display_ = nullptr;
    std::atomic<bool> test_requested_{false};
    std::atomic<bool> vision_ready_{false};
    std::atomic<bool> interaction_requested_{false};
    bool started_ = false;

    static void TaskEntry(void* arg);
    void Run();

public:
    void Start(EspVideo* camera, QdtechTab5Display* display);
    void RequestPresenceTest();
    void NotifyInteraction() { interaction_requested_.store(true); }
};
