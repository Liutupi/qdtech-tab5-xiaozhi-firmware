#pragma once
#include "nabo_three_state.h"
namespace nabo_scene {
struct SceneInput {
    bool speaking, playback, sleeping, working, listening;
    bool voice, music, hidden, gesture;
};
struct Selection {
    int clip;
    unsigned frame;
};
inline Selection Select(uint64_t now, Output scene, SceneInput input) {
    if (input.hidden || input.gesture || input.voice || input.music || input.playback ||
        input.speaking)
        return {-1, 0};
    if (scene.state == State::EnterSleep || scene.state == State::Waking)
        return {1, scene.frame};
    if (scene.state == State::Sleeping)
        return {2, scene.frame};
    if (input.working)
        return {3, unsigned((now / 40) % 50)};
    if (!input.listening)
        return {0, scene.frame};
    return {-1, 0};
}
}  // namespace nabo_scene
