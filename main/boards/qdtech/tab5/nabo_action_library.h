#pragma once
#include <array>
#include <cstdint>
#include "nabo_scene_policy.h"

namespace nabo_scene {
struct ActionClip {
    const char* id;
    unsigned frames;
    unsigned weight;
};
inline constexpr std::array<ActionClip, 26> kActions{{
    {"A06", 125, 1},   {"A09", 125, 1},   {"A16", 125, 2},   {"E04", 100, 1},   {"E05", 100, 4},
    {"E06", 100, 5},   {"E09", 100, 3},   {"E14", 100, 2},   {"P01", 125, 1},   {"P02", 125, 1},
    {"A03R1", 125, 1}, {"A05R1", 125, 3}, {"E12R1", 100, 2}, {"E03R2", 100, 3}, {"A07", 126, 1},
    {"A08", 126, 2},   {"A10", 126, 2},   {"E10", 125, 2},   {"A03", 125, 1},   {"A05", 125, 2},
    {"A12", 126, 1},   {"E13", 125, 1},   {"A13", 125, 1},   {"P04", 125, 1},   {"A14", 125, 1},
    {"E15", 125, 2},
}};
inline constexpr unsigned kClipCount = 4 + kActions.size();
static_assert(kClipCount <= 32, "Clip failure masks have 32 bits");

// Visual-only, allocation-free. One action at a time, real audio always wins.
// Decorative clips cannot delay a reply or change the application's state.
class ActionLibrary {
public:
    // Asks for one clip by id (e.g. a 每日推送 reminder). It starts at the next neutral
    // idle boundary, ahead of the random pick and its cooldowns; whatever blocks idle
    // actions (voice, music, sleep, apps) also holds it, and it lapses after a minute.
    bool Request(const char* id, uint64_t now) {
        for (unsigned i = 0; i < kActions.size(); ++i) {
            const char* a = kActions[i].id;
            const char* b = id;
            while (*a && *a == *b)
                ++a, ++b;
            if (*a == 0 && *b == 0) {
                requested_ = int(i);
                requested_until_ = now + 60000;
                return true;
            }
        }
        return false;
    }

    Selection Apply(uint64_t now, Selection base, SceneInput input, uint32_t failed, bool enabled) {
        const bool entered_listening = input.listening && !listening_;
        const bool entered_work = input.working && !working_;
        listening_ = input.listening;
        working_ = input.working;
        const bool blocked = input.hidden || input.gesture || input.voice || input.music ||
                             input.playback || input.speaking || input.sleeping || base.clip == 1 ||
                             base.clip == 2;
        if (!enabled || blocked) {
            Stop(now);
            idle_ = false;
            return base;
        }
        if (!initialized_) {
            initialized_ = true;
            random_ ^= uint32_t(now);
            Schedule(now);
        }
        if (active_ >= 0) {
            const unsigned clip = unsigned(active_) + 4;
            const bool context_changed =
                (context_ == 1 && !input.listening) || (context_ == 2 && !input.working);
            if (context_changed || (failed & (1u << clip)) ||
                now - since_ >= kActions[active_].frames * 40) {
                Stop(now);
                idle_since_ = now;
            } else {
                return {int(clip), unsigned((now - since_) / 40)};
            }
        }
        if (entered_work && Start(11, now, failed, 2))
            return {active_ + 4, 0};
        if (entered_listening && !input.working && Start(2, now, failed, 1))
            return {active_ + 4, 0};
        if (base.clip != 0) {
            idle_ = false;
            return base;
        }
        if (!idle_) {
            idle_ = true;
            idle_since_ = now;
            Schedule(now);
        }
        const unsigned frame = unsigned((now - idle_since_) / 40) % 113;
        if (requested_ >= 0 && frame == 0) {
            const int wanted = requested_;
            requested_ = -1;
            if (now <= requested_until_ && !(failed & (1u << (wanted + 4)))) {
                active_ = last_ = wanted;
                context_ = 0;
                since_ = now;
                used_[wanted] = now ? now : 1;
                return {active_ + 4, 0};
            }
        }
        // Start at the neutral boundary, rather than jumping out of a blink.
        if (now >= next_ && frame == 0) {
            unsigned weight = 0;
            for (unsigned i = 0; i < kActions.size(); ++i)
                if (Eligible(i, now, failed))
                    weight += kActions[i].weight;
            if (weight) {
                unsigned pick = Random() % weight;
                for (unsigned i = 0; i < kActions.size(); ++i) {
                    if (!Eligible(i, now, failed))
                        continue;
                    if (pick < kActions[i].weight && Start(i, now, failed, 0))
                        return {active_ + 4, 0};
                    pick -= kActions[i].weight;
                }
            }
            Schedule(now);
        }
        return {0, frame};
    }

private:
    uint32_t Random() {
        random_ = random_ * 1664525u + 1013904223u;
        return random_;
    }
    void Schedule(uint64_t now) { next_ = now + 15000 + Random() % 15001; }
    bool Eligible(unsigned i, uint64_t now, uint32_t failed) const {
        return !(failed & (1u << (i + 4))) && int(i) != last_ &&
               (!used_[i] || now - used_[i] >= 600000);
    }
    bool Start(unsigned i, uint64_t now, uint32_t failed, unsigned context) {
        if (!Eligible(i, now, failed))
            return false;
        active_ = int(i);
        last_ = active_;
        context_ = context;
        since_ = now;
        used_[i] = now ? now : 1;
        return true;
    }
    void Stop(uint64_t now) {
        if (active_ >= 0)
            Schedule(now);
        active_ = -1;
    }
    std::array<uint64_t, kActions.size()> used_{};
    uint64_t since_ = 0, next_ = 0, idle_since_ = 0;
    uint32_t random_ = 0x4e41424f;
    int active_ = -1, last_ = -1, requested_ = -1;
    uint64_t requested_until_ = 0;
    unsigned context_ = 0;
    bool initialized_ = false, listening_ = false, working_ = false, idle_ = false;
};
}  // namespace nabo_scene
