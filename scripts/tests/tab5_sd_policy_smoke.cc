#include <cassert>
#include <cstdio>
#include "nabo_scene_policy.h"
using namespace nabo_scene;
int main() {
    Output frame{State::Idle, 12, 0, 0, 255, 1, false};
    SceneInput input{};
    assert(Select(480, frame, input).clip == 0);
    input.working = true;
    assert(Select(2040, frame, input).clip == 3 && Select(2040, frame, input).frame == 1);
    input.working = false;
    input.listening = true;
    assert(Select(480, frame, input).clip == -1);
    input.listening = false;
    for (auto state : {State::Idle, State::EnterSleep, State::Sleeping, State::Waking}) {
        frame.state = state;
        for (int bit = 0; bit < 6; ++bit) {
            input = {};
            switch (bit) {
                case 0:
                    input.speaking = true;
                    break;
                case 1:
                    input.playback = true;
                    break;
                case 2:
                    input.voice = true;
                    break;
                case 3:
                    input.music = true;
                    break;
                case 4:
                    input.hidden = true;
                    break;
                case 5:
                    input.gesture = true;
                    break;
            }
            assert(Select(0, frame, input).clip == -1);
        }
    }
    input = {};
    frame.state = State::EnterSleep;
    assert(Select(0, frame, input).clip == 1);
    frame.state = State::Sleeping;
    assert(Select(0, frame, input).clip == 2);
    frame.state = State::Waking;
    assert(Select(0, frame, input).clip == 1);
    const std::array<uint16_t, 207> matches{};
    Controller scene(matches);
    scene.Tick(0, {false, false, true});
    scene.Tick(400, {});
    assert(scene.Tick(440, {false, false, false, true}).state == State::Idle);
    std::puts(
        "PASS 24 cross-state audio/music/app/camera/gesture preemption cases; work event routing; "
        "listening static; early wake cancels entry");
}
