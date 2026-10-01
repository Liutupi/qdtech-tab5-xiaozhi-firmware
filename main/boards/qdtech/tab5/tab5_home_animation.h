#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

// No LVGL, allocation, IO or audio processing here. Absolute time lets the UI
// skip missed frames instead of stretching an action while decoding/rotating.
namespace tab5_home {
enum class State { Idle, Listening, Thinking, Speaking };

struct Frame {
    unsigned eyes = 0;  // 0: original portrait, 1: half, 2: closed
    unsigned mouth = 0;
    uint8_t mouth_opa = 255;
    std::array<int, 5> bars = {};
    std::array<int, 3> ambient_y = {};
    std::array<uint8_t, 3> ambient_opa = {};
    std::array<uint8_t, 3> thought_opa = {};
};

class Animation {
public:
    void SetState(State state, uint32_t now) {
        if (state == state_)
            return;
        state_ = state;
        state_since_ = now;
        // A fresh attentive blink, without restarting on repeated statuses.
        blink_start_ = now;
        next_blink_ = now + 2400;
        blinking_ = state == State::Listening;
    }

    State state() const { return state_; }

    void ResetEyes(uint32_t now) {
        blinking_ = false;
        next_blink_ = now + 2400;
    }

    Frame Sample(uint32_t now, bool voice, bool playback, bool greeting = false) {
        Frame frame;
        const uint32_t dt = sampled_ ? std::min<uint32_t>(now - last_sample_, 250) : 50;
        sampled_ = true;
        last_sample_ = now;
        const uint32_t age = now - state_since_;
        if (!blinking_ && static_cast<int32_t>(now - next_blink_) >= 0) {
            blink_start_ = now;
            blinking_ = true;
        }
        if (blinking_) {
            const uint32_t blink_age = now - blink_start_;
            frame.eyes = blink_age < 60 ? 1 : blink_age < 140 ? 2 : blink_age < 200 ? 1 : 0;
            if (blink_age >= 200) {
                blinking_ = false;
                next_blink_ = now + 3200 + (now * 37u) % 2300;
            }
        }
        // Brief half-lid glance while waiting for a reply, using original art.
        if (!frame.eyes && state_ == State::Thinking && age % 3600 < 350)
            frame.eyes = 1;

        // A varied 1.8 s visual cadence, gated by playback drain state. This is
        // not phoneme recognition; pauses close the mouth immediately.
        if ((state_ == State::Speaking && playback) || greeting) {
            static constexpr unsigned mouths[] = {0, 1, 2, 1, 0, 1, 1, 2, 1, 0, 1, 2,
                                                  2, 1, 0, 0, 1, 2, 1, 0, 1, 1, 0, 0};
            const uint32_t beat = age % 1800;
            frame.mouth = mouths[beat / 75];
            frame.mouth_opa =
                static_cast<uint8_t>(170 + std::min<uint32_t>(beat % 75, 50) * 85 / 50);
        }
        for (unsigned i = 0; i < frame.bars.size(); ++i) {
            int target = 10 + static_cast<int>(i == 2 ? 4 : 0);
            switch (state_) {
                case State::Idle:
                    target += Pulse(now + i * 320, 4200) / 32;
                    break;
                case State::Listening:
                    target += Pulse(age + i * 110, voice ? 520 : 1600) * (voice ? 30 : 10) / 256;
                    break;
                case State::Thinking:
                    target += Pulse(age + i * 230, 1700) * 12 / 256;
                    break;
                case State::Speaking:
                    target += playback ? Pulse(age + i * 140, 660 + i * 70) * 30 / 256 : 0;
                    break;
            }
            // A 200 ms response smooths state changes and VAD edges. Fixed
            // point avoids rounding stalls and has no floating-point cost.
            const int weight = static_cast<int>(std::min<uint32_t>(dt, 200));
            bar_q8_[i] += ((target * 256 - bar_q8_[i]) * weight) / 200;
            frame.bars[i] = (bar_q8_[i] + 128) / 256;
        }
        for (unsigned i = 0; i < 3; ++i) {
            const int breath = Pulse(now + i * 850, state_ == State::Idle ? 4400 : 2600);
            frame.ambient_y[i] = 112 + static_cast<int>(i) * 69 - breath / 64;
            frame.ambient_opa[i] = static_cast<uint8_t>(65 + breath * 90 / 256);
            frame.thought_opa[i] =
                state_ == State::Thinking
                    ? static_cast<uint8_t>(45 + Pulse(age + i * 300, 1200) *
                                                    std::min<uint32_t>(age, 300) / 400)
                    : 0;
        }
        return frame;
    }

    static int WaveFrame(uint32_t elapsed) {
        if (elapsed >= 1400)
            return -1;
        return elapsed < 200 || elapsed >= 1100 ? 0 : elapsed < 500 || elapsed >= 800 ? 1 : 2;
    }

private:
    // Raised-cosine approximation from a tiny symmetric lookup table.
    static int Pulse(uint32_t time, uint32_t period) {
        static constexpr int values[] = {0,   10,  37,  79,  128, 177, 219, 246, 256,
                                         246, 219, 177, 128, 79,  37,  10,  0};
        const uint32_t phase = (time % period) * 16;
        const unsigned index = phase / period;
        return values[index] + (values[index + 1] - values[index]) *
                                   static_cast<int>(phase % period) / static_cast<int>(period);
    }

    State state_ = State::Idle;
    uint32_t state_since_ = 0;
    uint32_t blink_start_ = 0;
    uint32_t next_blink_ = 3250;
    uint32_t last_sample_ = 0;
    bool blinking_ = false;
    bool sampled_ = false;
    std::array<int, 5> bar_q8_ = {12 * 256, 12 * 256, 14 * 256, 12 * 256, 12 * 256};
};
}  // namespace tab5_home
