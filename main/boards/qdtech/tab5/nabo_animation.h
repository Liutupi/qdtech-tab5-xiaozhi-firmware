#pragma once

#include <cmath>
#include <cstdint>

namespace nabo {

enum class Action { Idle, Wave, Happy, Wink, Encourage, Think, Curious, Listen, Comfort, Music };

// Positions are 1/256 pixel; rotations use LVGL's tenths of a degree.
struct WaveMotion {
    int body_x = 0, body_y = 0, torso_angle = 0, head_angle = 0, head_y = 0, hand_angle = 0;
};
struct LayerGeometry {
    int x, y, pivot_x, pivot_y;
};
struct WaveGeometry {
    LayerGeometry legs, torso, head, hand, cuff;
    int hand_rest_angle;
};
struct LayerPose {
    int x = 0, y = 0, angle = 0;
};
struct WavePose {
    LayerPose legs, torso, head, hand, cuff;
};

inline int Pixel(int fixed) { return static_cast<int>(std::lround(fixed / 256.0)); }

inline WavePose BuildWavePose(const WaveGeometry& geometry, const WaveMotion& motion) {
    const int hip_x = geometry.torso.x + geometry.torso.pivot_x;
    const int hip_y = geometry.torso.y + geometry.torso.pivot_y;
    const double angle = motion.torso_angle * 3.14159265358979323846 / 1800;
    const double cosine = std::cos(angle), sine = std::sin(angle);
    auto follow_torso = [&](const LayerGeometry& part, int extra_angle, int extra_y) {
        const int dx = part.x + part.pivot_x - hip_x;
        const int dy = part.y + part.pivot_y - hip_y;
        return LayerPose{
            static_cast<int>(std::lround((hip_x + cosine * dx - sine * dy - part.pivot_x) * 256)) +
                motion.body_x,
            static_cast<int>(std::lround((hip_y + sine * dx + cosine * dy - part.pivot_y) * 256)) +
                motion.body_y + extra_y,
            motion.torso_angle + extra_angle};
    };
    return {{geometry.legs.x * 256, geometry.legs.y * 256, 0},
            follow_torso(geometry.torso, 0, 0),
            follow_torso(geometry.head, motion.head_angle, motion.head_y),
            follow_torso(geometry.hand, geometry.hand_rest_angle + motion.hand_angle, 0),
            follow_torso(geometry.cuff, 0, 0)};
}

// Wall-clock choreography: delayed UI callbacks skip to the current frame
// instead of stretching an animation or replaying a backlog of frames.
class Animation {
public:
    void Start(Action action, uint64_t now_ms) {
        action_ = action;
        started_ms_ = now_ms;
    }

    void Stop() { action_ = Action::Idle; }

    Action Current(uint64_t now_ms) const {
        return now_ms - started_ms_ < Duration(action_) ? action_ : Action::Idle;
    }

    int Frame(uint64_t now_ms) const {
        const auto action = Current(now_ms);
        if (action == Action::Idle)
            return -1;
        if (action == Action::Wave)
            return 0;  // Registered layers share one continuous full-body motion.
        return static_cast<int>(action) - static_cast<int>(Action::Happy);
    }

    // LVGL angles use tenths of a degree. The same samples drive the preview.
    int WaveAngle(uint64_t now_ms) const {
        if (Current(now_ms) != Action::Wave)
            return 0;
        const auto age = now_ms - started_ms_;
        static constexpr int sine[] = {0,   98,   181,  237,  256,  237,  181, 98, 0,
                                       -98, -181, -237, -256, -237, -181, -98, 0};
        const unsigned phase = (age % 800) * 16;
        const unsigned index = phase / 800;
        const int value =
            sine[index] + (sine[index + 1] - sine[index]) * static_cast<int>(phase % 800) / 800;
        // Ease into and out of the wave with zero speed at both ends.
        const uint64_t edge = age < 240                            ? age
                              : Duration(Action::Wave) - age < 240 ? Duration(Action::Wave) - age
                                                                   : 240;
        const int progress = edge * 256 / 240;
        const int envelope = progress * progress * (768 - 2 * progress) / 65536;
        return value * 80 * envelope / 65536;
    }

    static constexpr bool HasMotion(Action action) {
        return action >= Action::Wave && action <= Action::Music;
    }

    WaveMotion Motion(uint64_t now_ms) const {
        const auto action = Current(now_ms);
        if (action != Action::Wave && HasMotion(action))
            return ReactionMotion(action, now_ms - started_ms_);
        if (action != Action::Wave)
            return {};
        struct Key {
            uint64_t time;
            WaveMotion motion;
        };
        // Anticipation, greeting, delayed head response, then a quiet settle.
        static constexpr Key keys[] = {{0, {}},
                                       {180, {256, 256, 8, 0, 0, 0}},
                                       {480, {-768, 256, -24, 14, 256, 0}},
                                       {920, {-256, 0, -8, -28, 768, 0}},
                                       {1320, {768, 512, 18, -8, 256, 0}},
                                       {1740, {512, 256, 8, 20, 768, 0}},
                                       {2040, {-256, 256, -8, 10, 256, 0}},
                                       {2400, {}}};
        const uint64_t age = now_ms - started_ms_;
        unsigned index = 0;
        while (keys[index + 1].time < age)
            ++index;
        const auto& first = keys[index];
        const auto& last = keys[index + 1];
        const int progress = (age - first.time) * 256 / (last.time - first.time);
        const int eased = progress * progress * (768 - 2 * progress) / 65536;
        auto blend = [&](int a, int b) { return a + (b - a) * eased / 256; };
        return {blend(first.motion.body_x, last.motion.body_x),
                blend(first.motion.body_y, last.motion.body_y),
                blend(first.motion.torso_angle, last.motion.torso_angle),
                blend(first.motion.head_angle, last.motion.head_angle),
                blend(first.motion.head_y, last.motion.head_y),
                WaveAngle(now_ms)};
    }

    static constexpr uint64_t Duration(Action action) {
        return action == Action::Idle ? 0 : action == Action::Wave ? 2400 : 1800;
    }

private:
    static WaveMotion ReactionMotion(Action action, uint64_t age) {
        // Each reaction starts and settles at its registered pose. Arms stay
        // attached to the torso; the thinking hand keeps its chin contact.
        static constexpr uint64_t times[] = {0, 180, 450, 750, 1050, 1350, 1600, 1800};
        static constexpr WaveMotion happy[] = {{},
                                               {0, 256, -6, 0, 0, 0},
                                               {-256, -1280, -14, 18, -128, 0},
                                               {256, -256, 8, -10, 128, 0},
                                               {256, -1024, 12, -16, -128, 0},
                                               {-256, 0, -8, 10, 128, 0},
                                               {0, 256, 0, 0, 0, 0},
                                               {}};
        static constexpr WaveMotion think[] = {{},
                                               {0, 128, 4, 0, 0, 0},
                                               {256, 0, 14, 0, 0, 0},
                                               {256, 0, 18, 0, 0, 0},
                                               {-128, -128, -8, 0, 0, 0},
                                               {-128, 0, -12, 0, 0, 0},
                                               {0, 0, -4, 0, 0, 0},
                                               {}};
        static constexpr WaveMotion listen[] = {{},
                                                {0, 128, 5, 0, 0, 0},
                                                {-256, 0, -24, 8, 0, 0},
                                                {-256, 128, -28, 12, 128, 0},
                                                {-128, 0, -18, -6, 0, 0},
                                                {0, 128, -10, 6, 128, 0},
                                                {0, 0, -4, 0, 0, 0},
                                                {}};
        static constexpr WaveMotion music[] = {{},
                                               {128, 256, 6, 0, 0, 0},
                                               {-512, -384, -30, 18, 128, 0},
                                               {512, 0, 30, -22, -128, 0},
                                               {-512, -512, -28, 22, 128, 0},
                                               {384, 0, 24, -18, -128, 0},
                                               {-128, 128, -8, 8, 0, 0},
                                               {}};
        // The raised hands rotate only around their original wrist joints.
        static constexpr WaveMotion wink[] = {{},
                                              {0, 128, 5, 0, 0, 0},
                                              {-256, -256, -24, 0, 0, -40},
                                              {-128, 0, -12, 0, 0, 30},
                                              {256, -128, 14, 0, 0, -30},
                                              {128, 128, 8, 0, 0, 20},
                                              {0, 0, 2, 0, 0, 0},
                                              {}};
        static constexpr WaveMotion encourage[] = {{},
                                                   {0, 128, -4, 0, 0, 0},
                                                   {256, -384, 12, 0, 0, -35},
                                                   {0, 256, -8, 0, 0, 20},
                                                   {128, -256, 8, 0, 0, -20},
                                                   {0, 128, -4, 0, 0, 10},
                                                   {0, 0, 0, 0, 0, 0},
                                                   {}};
        static constexpr WaveMotion curious[] = {{},
                                                 {-128, 128, -6, 0, 0, 0},
                                                 {384, -128, 28, 0, 0, 0},
                                                 {256, 0, 24, 0, 0, 0},
                                                 {-384, -128, -26, 0, 0, 0},
                                                 {-256, 128, -18, 0, 0, 0},
                                                 {0, 0, -4, 0, 0, 0},
                                                 {}};
        static constexpr WaveMotion comfort[] = {{},
                                                 {0, 128, 2, 0, 0, 0},
                                                 {-128, -256, -10, 0, 0, 0},
                                                 {-128, 256, -6, 0, 0, 0},
                                                 {128, -256, 8, 0, 0, 0},
                                                 {128, 128, 4, 0, 0, 0},
                                                 {0, 0, 0, 0, 0, 0},
                                                 {}};
        const auto* keys = action == Action::Happy       ? happy
                           : action == Action::Think     ? think
                           : action == Action::Listen    ? listen
                           : action == Action::Music     ? music
                           : action == Action::Wink      ? wink
                           : action == Action::Encourage ? encourage
                           : action == Action::Curious   ? curious
                                                         : comfort;
        unsigned index = 0;
        while (times[index + 1] < age)
            ++index;
        const int progress = (age - times[index]) * 256 / (times[index + 1] - times[index]);
        const int eased = progress * progress * (768 - 2 * progress) / 65536;
        const auto& first = keys[index];
        const auto& last = keys[index + 1];
        auto blend = [&](int a, int b) { return a + (b - a) * eased / 256; };
        return {
            blend(first.body_x, last.body_x),           blend(first.body_y, last.body_y),
            blend(first.torso_angle, last.torso_angle), blend(first.head_angle, last.head_angle),
            blend(first.head_y, last.head_y),           blend(first.hand_angle, last.hand_angle)};
    }

    Action action_ = Action::Idle;
    uint64_t started_ms_ = 0;
};

}  // namespace nabo
