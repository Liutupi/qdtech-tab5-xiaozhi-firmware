#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include "tab5_home_animation.h"

namespace nabo_scene {
enum class State { Idle, Talking, EnterSleep, Sleeping, Waking };
struct Input {
    bool speaking = false;
    bool playback = false;
    bool sleep_request = false;
    bool wake_request = false;
};
struct Output {
    State state;
    unsigned frame;
    unsigned eyes;
    unsigned mouth;
    uint8_t mouth_opacity;
    uint32_t generation;
    bool wake_for_speech;
};

// Visual-only scheduler. It never starts/stops, queues, or delays real audio.
// Video and expression resources retain their separate ownership.
class Controller {
public:
    explicit Controller(const std::array<uint16_t, 207>& matches) : wake_matches_(matches) {}
    Output Tick(uint64_t now, Input input) {
        face_.SetState(input.speaking ? tab5_home::State::Speaking : tab5_home::State::Idle,
                       static_cast<uint32_t>(now));
        if (input.speaking)
            pending_sleep_ = false;
        else if (input.sleep_request)
            pending_sleep_ = true;

        if (input.speaking && state_ == State::Sleeping) {
            StartWake(now, wake_matches_[std::min(last_frame_, 206u)], true);
        } else if (input.speaking && state_ == State::EnterSleep) {
            if (last_frame_ <= 24)
                Change(State::Talking, now);
            else
                StartWake(now, last_frame_, true);
        } else if (input.speaking && state_ == State::Idle) {
            Change(State::Talking, now);
        } else if (state_ == State::Waking) {
            wake_for_speech_ = input.speaking;
        }

        if (input.wake_request && state_ == State::Sleeping)
            StartWake(now, wake_matches_[std::min(last_frame_, 206u)], input.speaking);
        if (input.wake_request && state_ == State::EnterSleep) {
            pending_sleep_ = false;
            if (last_frame_ <= 24)
                Change(input.speaking ? State::Talking : State::Idle, now);
            else
                StartWake(now, last_frame_, input.speaking);
        }
        if (state_ == State::Talking && !input.speaking && !input.playback)
            Change(State::Idle, now);
        if (state_ == State::Idle && pending_sleep_ && !input.speaking && !input.playback) {
            pending_sleep_ = false;
            Change(State::EnterSleep, now);
        }

        if (state_ == State::EnterSleep && now - since_ >= 1320)
            Change(State::Sleeping, since_ + 1320);
        if (state_ == State::Waking && now - since_ >= WakeDuration())
            Change(input.speaking ? State::Talking : State::Idle, since_ + WakeDuration());

        const uint64_t age = now - since_;
        unsigned frame = 0;
        switch (state_) {
            case State::Idle:
                frame = (age / 40) % 113;
                break;
            case State::Talking:
                frame = 0;
                break;
            case State::EnterSleep:
                frame = std::min<uint64_t>(age / 40, 32);
                break;
            case State::Sleeping:
                frame = (age / 40) % 207;
                break;
            case State::Waking:
                frame = std::max<int>(24, int(wake_start_) - int(age / 40) * 2);
                break;
        }
        last_frame_ = frame;
        const auto face =
            face_.Sample(static_cast<uint32_t>(now), false, input.speaking && input.playback);
        // Keep moving-video faces untouched. During waking, real audio remains
        // independent; visual mouth animation begins only after the stable pose.
        return {state_,
                frame,
                state_ == State::Talking ? face.eyes : 0,
                state_ == State::Talking ? face.mouth : 0,
                face.mouth_opa,
                generation_,
                wake_for_speech_};
    }
    State state() const { return state_; }
    uint32_t generation() const { return generation_; }
    uint64_t WakeDuration() const { return ((wake_start_ - 24 + 1) / 2 + 1) * 40; }

private:
    void Change(State next, uint64_t now) {
        state_ = next;
        since_ = now;
        last_frame_ = 0;
        ++generation_;
        if (next != State::Waking)
            wake_for_speech_ = false;
    }
    void StartWake(uint64_t now, unsigned start, bool for_speech) {
        wake_start_ = std::max(24u, std::min(start, 80u));
        Change(State::Waking, now);
        wake_for_speech_ = for_speech;
    }
    const std::array<uint16_t, 207>& wake_matches_;
    tab5_home::Animation face_;
    State state_ = State::Idle;
    uint64_t since_ = 0;
    unsigned last_frame_ = 0, wake_start_ = 24;
    uint32_t generation_ = 1;
    bool pending_sleep_ = false, wake_for_speech_ = false;
};
}  // namespace nabo_scene
