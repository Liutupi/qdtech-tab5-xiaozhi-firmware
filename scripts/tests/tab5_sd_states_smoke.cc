#include <cassert>
#include <cstdio>
#include "nabo_three_state.h"
using namespace nabo_scene;

int main() {
    std::array<uint16_t, 207> map;
    map.fill(72);
    for (unsigned i = 0; i < 48; ++i)
        map[i] = 33 + i;
    unsigned passed = 0;
    auto pass = [&](const char* s) {
        ++passed;
        std::printf("PASS %s\n", s);
    };
    {
        Controller c(map);
        assert(c.Tick(0, {}).frame == 0);
        assert(c.Tick(4480, {}).frame == 112);
        assert(c.Tick(4520, {}).frame == 0);
        assert(c.Tick((1ULL << 32) + 40, {}).frame < 113);
        pass("approved 113-frame idle clock unchanged, including long uptime");
    }
    {
        Controller c(map);
        c.Tick(0, {});
        auto a = c.Tick(400, {});
        auto b = c.Tick(440, {true, true});
        assert(b.state == State::Talking && b.generation > a.generation);
        const auto g = b.generation;
        bool mouth = false;
        for (uint64_t t = 480; t < 1000; t += 40) {
            auto r = c.Tick(t, {true, true});
            mouth |= r.mouth != 0;
            assert(r.generation == g);
        }
        assert(mouth);
        auto pause = c.Tick(1000, {true, false});
        assert(pause.mouth == 0);
        assert(c.Tick(1040, {true, false}).mouth == 0);
        assert(c.Tick(1080, {}).state == State::Idle);
        pass("speech preempts idle once; original playback/drain gate closes mouth immediately");
    }
    {
        Controller c(map);
        auto r = c.Tick(0, {false, false, true});
        assert(r.state == State::EnterSleep && r.frame == 0);
        r = c.Tick(1280, {});
        assert(r.state == State::EnterSleep && r.frame == 32);
        r = c.Tick(1320, {});
        assert(r.state == State::Sleeping && r.frame == 0);
        assert(c.Tick(9560, {}).frame == 206);
        assert(c.Tick(9600, {}).frame == 0);
        pass("real 33-frame sleep entry hands off to uncut 207-frame sleep asset");
    }
    {
        Controller c(map);
        c.Tick(0, {false, false, true});
        c.Tick(1320, {});
        c.Tick(2840, {});
        auto wake = c.Tick(2880, {true, true});
        assert(wake.state == State::Waking && wake.frame == 71 && wake.wake_for_speech);
        const auto generation = wake.generation;
        const auto duration = c.WakeDuration();
        assert(duration <= 1160);
        for (uint64_t t = 2920; t < 2880 + duration; t += 40) {
            auto r = c.Tick(t, {true, true});
            assert(r.state == State::Waking && r.mouth == 0 && r.generation == generation);
        }
        auto talk = c.Tick(2880 + duration, {true, true});
        assert(talk.state == State::Talking && talk.generation > generation);
        assert(c.Tick(2920 + duration, {true, false}).mouth == 0);
        pass("speech cancels sleep generation; bounded visual wake then gated speaking");
    }
    {
        Controller c(map);
        c.Tick(0, {false, false, true});
        c.Tick(1280, {});
        auto r = c.Tick(1320, {true, true});
        assert(r.state == State::Waking && r.frame == 32);
        auto duration = c.WakeDuration();
        for (uint64_t t = 1360; t <= 1320 + duration; t += 40)
            r = c.Tick(t, {});
        assert(r.state == State::Idle && r.mouth == 0);
        pass(
            "speech interrupted during entry then drained during wake does not cause stale mouth "
            "motion");
    }
    {
        Controller c(map);
        c.Tick(0, {false, false, true});
        c.Tick(1320, {});
        c.Tick(9560, {});
        auto r = c.Tick(9600, {false, false, false, true});
        assert(r.state == State::Waking && r.frame == 72);
        auto g = r.generation;
        r = c.Tick(9640, {true, true});
        assert(r.state == State::Waking && r.generation == g && r.wake_for_speech);
        pass("normal wake can retarget to speaking without restarting the visual transition");
    }
    {
        Controller c(map);
        c.Tick(0, {true, true});
        auto r = c.Tick(40, {true, true, true});
        assert(r.state == State::Talking);
        r = c.Tick(80, {});
        assert(r.state == State::Idle);
        r = c.Tick(120, {false, true, true});
        assert(r.state == State::Idle);
        r = c.Tick(160, {});
        assert(r.state == State::EnterSleep);
        pass("sleep requests never overtake active speech or playback");
    }
    std::printf("RESULT tests=%u controller_bytes=%zu audio_side_effects=none\n", passed,
                sizeof(Controller));
}
