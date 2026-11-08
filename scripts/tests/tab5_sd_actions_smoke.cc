#include <cassert>
#include <cstdio>
#include "nabo_action_library.h"
using namespace nabo_scene;

int main() {
    SceneInput input{};
    ActionLibrary library;
    uint64_t seen = 0, expected = 0;
    unsigned starts = 0, shorter_waits = 0;
    for (unsigned i = 0; i < kActions.size(); ++i)
        if (kActions[i].weight)
            expected |= uint64_t{1} << i;
    int previous = -1;
    uint64_t ended = 0;
    std::array<uint64_t, kActions.size()> last{};
    for (uint64_t t = 0; t < 14400000; t += 20) {
        const auto s = library.Apply(t, {0, 0}, input, 0, true);
        if (s.clip >= 4) {
            assert(s.clip < int(kClipCount));
            assert(s.frame < kActions[s.clip - 4].frames);
            if (s.clip != previous) {
                assert(s.frame == 0);
                if (starts) {
                    assert(t - ended >= 15000 && t - ended <= 30000 + 4520);
                    shorter_waits += t - ended < 20000;
                }
                if (last[s.clip - 4])
                    assert(t - last[s.clip - 4] >= 600000);
                last[s.clip - 4] = t;
                ++starts;
                seen |= uint64_t{1} << (s.clip - 4);
            }
        } else if (previous >= 4) {
            assert(s.clip == 0 && s.frame == 0);
            ended = t;
        }
        previous = s.clip;
    }
    assert(seen == expected && starts > 100);
    assert(shorter_waits > 0);
    ActionLibrary events;
    input.listening = true;
    assert(events.Apply(100, {-1, 0}, input, 0, true).clip == 6);
    input.voice = true;
    assert(events.Apply(120, {-1, 0}, input, 0, true).clip == -1);
    input = {};
    input.working = true;
    assert(events.Apply(200, {3, 0}, input, 0, true).clip == 15);
    input.speaking = input.playback = true;
    assert(events.Apply(220, {-1, 0}, input, 0, true).clip == -1);
    input = {};
    for (unsigned bit = 0; bit < 7; ++bit) {
        ActionLibrary cancel;
        SceneInput event{};
        event.working = true;
        assert(cancel.Apply(0, {3, 0}, event, 0, true).clip >= 4);
        switch (bit) {
            case 0:
                event.voice = true;
                break;
            case 1:
                event.music = true;
                break;
            case 2:
                event.playback = true;
                break;
            case 3:
                event.speaking = true;
                break;
            case 4:
                event.sleeping = true;
                break;
            case 5:
                event.hidden = true;
                break;
            case 6:
                event.gesture = true;
                break;
        }
        assert(cancel.Apply(20, {bit == 4 ? 1 : -1, 0}, event, 0, true).clip < 4);
    }
    ActionLibrary unavailable;
    for (uint64_t t = 0; t < 1000000; t += 20)
        assert(unavailable.Apply(t, {0, 0}, input, ~nabo_sd::FailedClips{0}, true).clip == 0);
    // A failed high index must not alias a low clip or disappear at bit 32.
    for (unsigned clip = 32; clip < kClipCount; ++clip) {
        if (!kActions[clip - 4].weight)
            continue;
        ActionLibrary high_only;
        bool started = false;
        const auto failed = ~nabo_sd::ClipBit(clip);
        for (uint64_t t = 0; t < 40000; t += 20) {
            auto selection = high_only.Apply(t, {0, 0}, input, failed, true);
            if (selection.clip >= 4) {
                assert(selection.clip == int(clip));
                started = true;
            }
        }
        assert(started);
    }
    // Keep v1.0.18 reminder requests, including high-index failure isolation.
    ActionLibrary requested;
    assert(!requested.Request(nullptr, 0));
    assert(!requested.Request("A18", 0));
    assert(requested.Request("E18", 0));
    assert(requested.Apply(0, {0, 0}, input, nabo_sd::ClipBit(5), true).clip == 37);
    ActionLibrary requested_failed;
    assert(requested_failed.Request("E18", 0));
    assert(requested_failed.Apply(0, {0, 0}, input, nabo_sd::ClipBit(37), true).clip == 0);
    ActionLibrary original_reminder;
    assert(original_reminder.Request("A07", 0));
    assert(original_reminder.Apply(0, {0, 0}, input, 0, true).clip == 18);
    ActionLibrary legacy;
    for (uint64_t t = 0; t < 1000000; t += 20)
        assert(legacy.Apply(t, {0, 7}, input, 0, false).frame == 7);
    std::puts(
        "PASS all-action coverage, idle spacing/cooldowns, exact neutral restart, audio and sleep "
        "preemption, missing-assets fallback");
}
