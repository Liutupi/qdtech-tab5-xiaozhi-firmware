#pragma once

#include <string_view>
#include "nabo_animation.h"

namespace nabo {

inline Action EmotionAction(std::string_view emotion) {
    if (emotion == "happy" || emotion == "laughing" || emotion == "loving")
        return Action::Happy;
    if (emotion == "funny" || emotion == "winking")
        return Action::Wink;
    if (emotion == "thinking" || emotion == "confused")
        return Action::Think;
    if (emotion == "surprised" || emotion == "shocked" || emotion == "curious")
        return Action::Curious;
    if (emotion == "confident" || emotion == "cool" || emotion == "encouraging")
        return Action::Encourage;
    if (emotion == "sad" || emotion == "crying" || emotion == "comforting")
        return Action::Comfort;
    return Action::Idle;
}

inline Action IdleAction(unsigned index) {
    static constexpr Action actions[] = {Action::Curious, Action::Think, Action::Wink};
    return actions[index % 3];
}

// A single recent emotion survives speech. Repeated messages replace it,
// and neutral status updates leave it intact at the end of a TTS response.
class PendingEmotion {
public:
    void Store(Action action, uint64_t now_ms) {
        if (action == Action::Idle)
            return;
        action_ = action;
        received_ms_ = now_ms;
    }

    void Clear() { action_ = Action::Idle; }

    Action Take(uint64_t now_ms, bool ready, bool speech_pending = false) {
        if (action_ == Action::Idle)
            return Action::Idle;
        if (speech_pending) {
            received_ms_ = now_ms;
            return Action::Idle;
        }
        if (now_ms - received_ms_ >= 15000) {
            Clear();
            return Action::Idle;
        }
        if (!ready)
            return Action::Idle;
        const auto action = action_;
        Clear();
        return action;
    }

private:
    Action action_ = Action::Idle;
    uint64_t received_ms_ = 0;
};

}  // namespace nabo
