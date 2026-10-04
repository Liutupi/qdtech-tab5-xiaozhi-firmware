#pragma once
#include "nabo_three_state.h"
namespace nabo_scene {
struct SceneInput {
    bool speaking, playback, sleeping, working, listening;
    bool voice, music, hidden, gesture;
};
struct Selection {
    int clip;
    unsigned frame;
};
// The VAD bit is only meaningful while capture runs: disabling voice processing
// resets the AFE without a VAD-off callback, so the bit can stay true after a
// conversation ends and would block the idle scene until the next capture.
inline bool VoiceBlocksScene(bool waiting, bool voice_detected, bool capture_running) {
    (void)waiting;
    return voice_detected && capture_running;
}
inline Selection Select(uint64_t now, Output scene, SceneInput input) {
    if (input.hidden || input.gesture || input.voice || input.music || input.playback ||
        input.speaking)
        return {-1, 0};
    if (scene.state == State::EnterSleep || scene.state == State::Waking)
        return {1, scene.frame};
    if (scene.state == State::Sleeping)
        return {2, scene.frame};
    if (input.working)
        return {3, unsigned((now / 40) % 50)};
    if (!input.listening)
        return {0, scene.frame};
    return {-1, 0};
}
// Starts the accepted 50-frame source at zero on every eligible work entry.
// Other clips keep their original clocks and generation semantics.
class WorkClock {
public:
    Selection Apply(uint64_t now, Selection selection) {
        if (selection.clip != 3) {
            active_ = false;
            return selection;
        }
        if (!active_) {
            active_ = true;
            since_ = now;
        }
        selection.frame = unsigned((now - since_) / 40) % 50;
        return selection;
    }

private:
    uint64_t since_ = 0;
    bool active_ = false;
};
}  // namespace nabo_scene
