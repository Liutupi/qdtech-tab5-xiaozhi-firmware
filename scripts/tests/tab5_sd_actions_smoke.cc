#include <cassert>
#include <cstdio>
#include "nabo_action_library.h"
using namespace nabo_scene;

int main() {
    SceneInput input{};
    ActionLibrary library;
    unsigned seen = 0, starts = 0;
    int previous = -1;
    uint64_t ended = 0;
    std::array<uint64_t, 14> last{};
    for (uint64_t t = 0; t < 7200000; t += 20) {
        const auto s = library.Apply(t, {0, 0}, input, 0, true);
        if (s.clip >= 4) {
            assert(s.clip < int(kClipCount));
            assert(s.frame < kActions[s.clip - 4].frames);
            if (s.clip != previous) {
                assert(s.frame == 0);
                if (starts)
                    assert(t - ended >= 20000);
                if (last[s.clip - 4])
                    assert(t - last[s.clip - 4] >= 600000);
                last[s.clip - 4] = t;
                ++starts;
                seen |= 1u << (s.clip - 4);
            }
        } else if (previous >= 4) {
            assert(s.clip == 0 && s.frame == 0);
            ended = t;
        }
        previous = s.clip;
    }
    assert(seen == (1u << 14) - 1 && starts > 100);
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
        assert(unavailable.Apply(t, {0, 0}, input, 0x3fff0, true).clip == 0);
    ActionLibrary legacy;
    for (uint64_t t = 0; t < 1000000; t += 20)
        assert(legacy.Apply(t, {0, 7}, input, 0, false).frame == 7);
    std::puts(
        "PASS 14-action coverage, idle spacing/cooldowns, exact neutral restart, audio and sleep "
        "preemption, missing-assets fallback");
}
