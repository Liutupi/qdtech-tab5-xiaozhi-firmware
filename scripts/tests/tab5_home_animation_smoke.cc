#include "tab5_home_animation.h"

#include <cassert>
#include <cstdint>

using tab5_home::Animation;
using tab5_home::State;

int main() {
    Animation animation;
    animation.SetState(State::Listening, 1000);
    assert(animation.Sample(1000, false, false).eyes == 1);
    assert(animation.Sample(1100, false, false).eyes == 2);
    animation.SetState(State::Listening, 1100);  // repeated update must not restart blink
    assert(animation.Sample(1200, false, false).eyes == 0);

    animation.SetState(State::Speaking, 2000);
    assert(animation.Sample(2150, false, true).mouth == 2);
    assert(animation.Sample(2151, false, false).mouth == 0);  // playback pause
    animation.SetState(State::Idle, 2200);
    assert(animation.Sample(2200, false, true).mouth == 0);  // interrupted speech

    Animation smooth;
    const auto before = smooth.Sample(0, false, false);
    smooth.SetState(State::Listening, 0);
    const auto after = smooth.Sample(50, true, false);
    for (unsigned i = 0; i < before.bars.size(); ++i)
        assert(after.bars[i] - before.bars[i] <= 8);

    animation.SetState(State::Thinking, 3000);
    const auto thinking = animation.Sample(3500, false, false);
    assert(thinking.mouth == 0);
    assert(thinking.thought_opa[0] != thinking.thought_opa[1]);
    animation.SetState(State::Idle, 3500);
    for (auto opacity : animation.Sample(3500, false, false).thought_opa)
        assert(opacity == 0);

    // A delayed callback jumps to the current pose; it never replays old beats.
    assert(Animation::WaveFrame(0) == 0);
    assert(Animation::WaveFrame(650) == 2);
    assert(Animation::WaveFrame(1400) == -1);
    assert(Animation::WaveFrame(30000) == -1);
    animation.SetState(State::Listening, 4000);
    assert(animation.Sample(4020, false, false).eyes != 0);
    assert(animation.Sample(5000, false, false).eyes == 0);

    // Unsigned subtraction keeps short actions correct across LVGL tick wrap.
    constexpr uint32_t near_wrap = UINT32_MAX - 100;
    Animation wrap;
    wrap.SetState(State::Listening, near_wrap);
    assert(wrap.Sample(near_wrap + 100u, false, false).eyes == 2);
    assert(wrap.Sample(near_wrap + 250u, false, false).eyes == 0);

    for (auto state : {State::Idle, State::Listening, State::Thinking, State::Speaking}) {
        Animation long_run;
        long_run.SetState(state, 0);
        for (uint32_t now = 0; now < 86400000; now += 397) {
            const auto frame = long_run.Sample(now, now % 3 == 0, now % 5 != 0);
            assert(frame.eyes <= 2 && frame.mouth <= 2);
            for (int bar : frame.bars)
                assert(bar >= 10 && bar <= 44);
            for (unsigned i = 0; i < 3; ++i) {
                assert(frame.ambient_y[i] >= 108 + static_cast<int>(i) * 69);
                assert(frame.ambient_y[i] <= 112 + static_cast<int>(i) * 69);
            }
            if (state != State::Speaking)
                assert(frame.mouth == 0);
        }
    }
    static_assert(sizeof(Animation) <= 64, "animation must remain a small fixed-size controller");
}
