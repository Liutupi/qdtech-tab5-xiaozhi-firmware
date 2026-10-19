#pragma once

#include <cstdint>

// NABO FM: which DJ action the radio studio shows for the current radio state.
// Host-tested; no LVGL, FreeRTOS or file access. Times are milliseconds.
namespace nabo_radio {

enum class Action : uint8_t {
    IdleOff,
    Wave,
    PhonesOn,
    PhonesOff,
    Tune,
    Groove,
    Sing,
    Listen,
    Sip,
    Love,
    Oops,
    Doze,
    Search,
    Count,
};
constexpr int kActionCount = static_cast<int>(Action::Count);

// File stems under /sdcard/tab5/nabo/radio-v1, in Action order.
// Must match ACTIONS in scripts/prepare_nabo_radio_actions.py.
inline const char* ActionName(Action action) {
    static const char* const kNames[kActionCount] = {
        "idle_off", "wave", "phones_on", "phones_off", "tune", "groove", "sing",
        "listen",   "sip",  "love",      "oops",       "doze", "search"};
    const int i = static_cast<int>(action);
    return i >= 0 && i < kActionCount ? kNames[i] : "idle_off";
}

inline bool IsOneShot(Action action) {
    switch (action) {
        case Action::Wave:
        case Action::PhonesOn:
        case Action::PhonesOff:
        case Action::Sip:
        case Action::Love:
        case Action::Oops:
            return true;
        default:
            return false;
    }
}

enum class Link : uint8_t { Stopped, Connecting, Playing, Error, NoNetwork };

struct Mood {
    Link link = Link::Stopped;
    bool music = false;   // music station or a requested song
    bool tuning = false;  // the dial is being dragged or a new station is pending
    bool dozing = false;  // last minute of a sleep timer
};

class Director {
public:
    static constexpr uint32_t kDefaultShotMs = 3000;
    static constexpr uint32_t kGrooveSwapMs = 12000;
    static constexpr uint32_t kSipMinMs = 60000, kSipSpanMs = 60000;

    // A one-shot pack may be shorter or longer than the default.
    void SetShotLength(Action action, uint32_t ms) {
        const int i = static_cast<int>(action);
        if (i >= 0 && i < kActionCount && ms >= 200 && ms <= 10000)
            shot_ms_[i] = ms;
    }
    // Opening the page starts from the current state; changes made while it
    // was hidden are not replayed as transitions.
    void Open(uint32_t now) {
        seen_ = false;
        Shot(Action::Wave, now);
    }
    void Favorite(uint32_t now) { Shot(Action::Love, now); }

    Action Update(const Mood& mood, uint32_t now) {
        if (!seen_) {
            // The first observation is a baseline, not a transition.
            seen_ = true;
            link_ = mood.link;
            ScheduleSip(now);
        } else if (mood.link != link_) {
            const bool was_on = On(link_), is_on = On(mood.link);
            if (mood.link == Link::Error)
                Shot(Action::Oops, now);
            else if (!was_on && is_on && link_ != Link::Error)
                Shot(Action::PhonesOn, now);
            else if (was_on && mood.link == Link::Stopped)
                Shot(Action::PhonesOff, now);
            if (mood.link == Link::Playing)
                ScheduleSip(now);
            link_ = mood.link;
        }
        if (shot_until_ && int32_t(now - shot_until_) < 0)
            return shot_;
        shot_until_ = 0;
        if (mood.link == Link::Playing && !mood.tuning && int32_t(now - next_sip_) >= 0) {
            ScheduleSip(now);
            Shot(Action::Sip, now);
            return shot_;
        }
        return Loop(mood, now);
    }

    Action Current() const { return shot_until_ ? shot_ : Action::IdleOff; }
    // Changes whenever a one-shot starts, also when the same shot repeats.
    uint32_t ShotSerial() const { return shot_serial_; }

private:
    static bool On(Link link) { return link == Link::Connecting || link == Link::Playing; }

    void Shot(Action action, uint32_t now) {
        shot_ = action;
        ++shot_serial_;
        shot_until_ = now + shot_ms_[static_cast<int>(action)];
        if (!shot_until_)
            shot_until_ = 1;  // 0 means "no shot"
    }

    void ScheduleSip(uint32_t now) {
        seed_ = seed_ * 1664525u + 1013904223u;
        next_sip_ = now + kSipMinMs + (seed_ >> 8) % kSipSpanMs;
    }

    Action Loop(const Mood& mood, uint32_t now) const {
        if (mood.link == Link::NoNetwork)
            return Action::Search;
        if (mood.dozing)
            return Action::Doze;
        if (mood.tuning || mood.link == Link::Connecting)
            return Action::Tune;
        if (mood.link == Link::Playing) {
            if (!mood.music)
                return Action::Listen;
            return (now / kGrooveSwapMs) % 2 ? Action::Sing : Action::Groove;
        }
        return Action::IdleOff;
    }

    uint32_t shot_ms_[kActionCount] = {
        kDefaultShotMs, kDefaultShotMs, kDefaultShotMs, kDefaultShotMs, kDefaultShotMs,
        kDefaultShotMs, kDefaultShotMs, kDefaultShotMs, kDefaultShotMs, kDefaultShotMs,
        kDefaultShotMs, kDefaultShotMs, kDefaultShotMs};
    Action shot_ = Action::IdleOff;
    uint32_t shot_until_ = 0;
    uint32_t next_sip_ = 0;
    uint32_t shot_serial_ = 0;
    uint32_t seed_ = 0x4e41424fu;
    Link link_ = Link::Stopped;
    bool seen_ = false;
};

// Maps RadioService state strings ("Playing", "Connecting", ...) to a Link.
inline Link LinkFromState(const char* state) {
    if (!state)
        return Link::Stopped;
    auto is = [state](const char* s) {
        const char* a = state;
        while (*a && *a == *s) {
            ++a;
            ++s;
        }
        return *a == *s;
    };
    if (is("Playing"))
        return Link::Playing;
    if (is("Connecting") || is("Reconnecting") || is("Buffering"))
        return Link::Connecting;
    if (is("Error") || is("Unavailable"))
        return Link::Error;
    if (is("Waiting WiFi"))
        return Link::NoNetwork;
    return Link::Stopped;
}

}  // namespace nabo_radio
