#include "nabo_animation.h"
#include "nabo_reactions.h"

#include <cassert>
#include <initializer_list>

int main() {
    nabo::Animation animation;
    assert(animation.Frame(1000) == -1);
    animation.Start(nabo::Action::Wave, 1000);
    assert(animation.Frame(1000) == 0);
    assert(animation.Frame(1099) == 0);
    assert(animation.Frame(1100) == 0);
    // A delayed render skips straight to its current frame.
    assert(animation.Frame(1450) == 0);
    assert(animation.Frame(2299) == 0);
    assert(animation.Current(3400) == nabo::Action::Idle);
    assert(animation.Frame(10000) == -1);
    assert(animation.WaveAngle(1000) == 0);
    assert(animation.WaveAngle(2000) == 80);
    assert(animation.WaveAngle(1600) == -80);
    assert(animation.WaveAngle(3400) == 0);
    int previous = 0;
    for (uint64_t now = 1000; now <= 3400; now += 20) {
        const int angle = animation.WaveAngle(now);
        assert(angle >= -80 && angle <= 80);
        const int delta = angle - previous;
        assert(delta >= -15 && delta <= 15);
        previous = angle;
    }
    // Absolute time also works beyond the 32-bit millisecond wrap.
    animation.Start(nabo::Action::Wave, (1ULL << 32) + 1000);
    assert(animation.WaveAngle((1ULL << 32) + 2000) == 80);
    constexpr nabo::WaveGeometry rig = {{57, 380, 93, 181}, {14, 222, 136, 205}, {20, 0, 131, 235},
                                        {-3, 183, 40, 67},  {10, 228, 27, 22},   -200};
    animation.Start(nabo::Action::Wave, 0);
    const auto neutral = nabo::BuildWavePose(rig, animation.Motion(0));
    bool body_moved = false, head_followed = false;
    for (uint64_t now = 0; now <= nabo::Animation::Duration(nabo::Action::Wave); now += 20) {
        const auto motion = animation.Motion(now);
        const auto pose = nabo::BuildWavePose(rig, motion);
        // Shoes stay on the floor, while the torso and head can move independently.
        assert(pose.legs.x == neutral.legs.x && pose.legs.y == neutral.legs.y);
        assert(pose.legs.angle == 0);
        body_moved |= pose.torso.angle != 0;
        head_followed |= motion.head_angle != 0 && pose.head.angle != pose.torso.angle;
        // The hand and cuff must have the exact same world-space wrist joint.
        const int wrist_x = pose.hand.x + rig.hand.pivot_x * 256;
        const int wrist_y = pose.hand.y + rig.hand.pivot_y * 256;
        assert(wrist_x == pose.cuff.x + rig.cuff.pivot_x * 256);
        assert(wrist_y == pose.cuff.y + rig.cuff.pivot_y * 256);
        // Torso rotation must not stretch the hip-to-wrist chain.
        const double dx = wrist_x / 256.0 - 150 - motion.body_x / 256.0;
        const double dy = wrist_y / 256.0 - 427 - motion.body_y / 256.0;
        assert(std::abs(std::hypot(dx, dy) - std::hypot(113.0, 177.0)) < 0.01);
    }
    assert(body_moved && head_followed);
    const auto settled = nabo::BuildWavePose(rig, animation.Motion(2400));
    assert(settled.torso.x == neutral.torso.x && settled.torso.y == neutral.torso.y);
    assert(settled.head.x == neutral.head.x && settled.head.angle == neutral.head.angle);
    animation.Start(nabo::Action::Wave, 2500);
    assert(animation.Motion(3000).torso_angle != 0);
    animation.Stop();
    const auto canceled = animation.Motion(3000);
    assert(canceled.torso_angle == 0 && canceled.head_angle == 0 && canceled.hand_angle == 0);
    // New reaction motion must vary, settle, and preserve the registered arms.
    const uint64_t reaction_start = (1ULL << 32) + 20000;
    for (const auto action : {nabo::Action::Happy, nabo::Action::Think, nabo::Action::Listen,
                              nabo::Action::Music, nabo::Action::Wink, nabo::Action::Encourage,
                              nabo::Action::Curious, nabo::Action::Comfort}) {
        assert(nabo::Animation::HasMotion(action));
        animation.Start(action, reaction_start);
        bool moved = false;
        auto previous_motion = animation.Motion(reaction_start);
        assert(previous_motion.body_x == 0 && previous_motion.torso_angle == 0);
        for (uint64_t age = 0; age <= nabo::Animation::Duration(action); age += 20) {
            const auto motion = animation.Motion(reaction_start + age);
            moved |= motion.torso_angle != 0 || motion.body_y != 0;
            assert(std::abs(motion.torso_angle) <= 30);
            assert(std::abs(motion.body_y) <= 1280);
            assert(std::abs(motion.torso_angle - previous_motion.torso_angle) <= 8);
            assert(std::abs(motion.body_y - previous_motion.body_y) <= 256);
            assert(std::abs(motion.hand_angle) <= 40);
            assert(std::abs(motion.hand_angle - previous_motion.hand_angle) <= 15);
            if (action != nabo::Action::Wink && action != nabo::Action::Encourage)
                assert(motion.hand_angle == 0);
            if (action == nabo::Action::Think)
                assert(motion.head_angle == 0 && motion.head_y == 0);
            previous_motion = motion;
        }
        assert(moved);
        const auto end = animation.Motion(reaction_start + nabo::Animation::Duration(action));
        assert(end.body_x == 0 && end.body_y == 0 && end.torso_angle == 0 && end.head_angle == 0);
        animation.Start(action, reaction_start);
        animation.Stop();
        assert(animation.Motion(reaction_start + 750).torso_angle == 0);
    }
    assert(!nabo::Animation::HasMotion(nabo::Action::Idle));
    assert(!nabo::Animation::HasMotion(static_cast<nabo::Action>(255)));
    for (const auto action : {nabo::Action::Wink, nabo::Action::Encourage}) {
        animation.Start(action, 0);
        assert(animation.Motion(450).hand_angle != 0);
        // Hand and foreground cuff stay attached even when the whole body sways.
        constexpr nabo::WaveGeometry wrist_rig = {{50, 418, 100, 143}, {15, 35, 135, 409},
                                                  {0, 0, 0, 0},        {23, 244, 47, 83},
                                                  {52, 319, 18, 8},    0};
        for (uint64_t t = 0; t < 1800; t += 20) {
            const auto pose = nabo::BuildWavePose(wrist_rig, animation.Motion(t));
            assert(pose.hand.x + wrist_rig.hand.pivot_x * 256 ==
                   pose.cuff.x + wrist_rig.cuff.pivot_x * 256);
            assert(pose.hand.y + wrist_rig.hand.pivot_y * 256 ==
                   pose.cuff.y + wrist_rig.cuff.pivot_y * 256);
        }
    }
    // TTS neutral updates must not erase the most recent reply emotion.
    nabo::PendingEmotion emotion;
    assert(nabo::EmotionAction("sad") == nabo::Action::Comfort);
    assert(nabo::EmotionAction("confident") == nabo::Action::Encourage);
    assert(nabo::EmotionAction("funny") == nabo::Action::Wink);
    assert(nabo::EmotionAction("surprised") == nabo::Action::Curious);
    assert(nabo::EmotionAction("neutral") == nabo::Action::Idle);
    assert(nabo::EmotionAction("unsupported") == nabo::Action::Idle);
    const uint64_t reply_time = (1ULL << 32) + 1000;
    emotion.Store(nabo::EmotionAction("sad"), reply_time);
    assert(emotion.Take(reply_time + 500, false) == nabo::Action::Idle);
    emotion.Store(nabo::EmotionAction("confident"), reply_time + 600);
    emotion.Store(nabo::EmotionAction("neutral"), reply_time + 800);
    assert(emotion.Take(reply_time + 1000, false) == nabo::Action::Idle);
    assert(emotion.Take(reply_time + 2000, true) == nabo::Action::Encourage);
    assert(emotion.Take(reply_time + 2100, true) == nabo::Action::Idle);
    emotion.Store(nabo::Action::Happy, reply_time);
    emotion.Clear();
    assert(emotion.Take(reply_time + 2200, true) == nabo::Action::Idle);
    emotion.Store(nabo::Action::Comfort, reply_time);
    assert(emotion.Take(reply_time + 15000, true) == nabo::Action::Idle);
    assert(emotion.Take(reply_time + 15100, true) == nabo::Action::Idle);
    // A long TTS response and late playback drain cannot age out its last emotion.
    emotion.Store(nabo::Action::Comfort, reply_time);
    assert(emotion.Take(reply_time + 20000, false, true) == nabo::Action::Idle);
    assert(emotion.Take(reply_time + 60000, false, true) == nabo::Action::Idle);
    emotion.Store(nabo::EmotionAction("neutral"), reply_time + 60010);
    assert(emotion.Take(reply_time + 62000, true) == nabo::Action::Comfort);
    // Idle gestures do not spontaneously select comfort or celebration.
    for (unsigned i = 0; i < 20; ++i) {
        const auto action = nabo::IdleAction(i);
        assert(action == nabo::Action::Curious || action == nabo::Action::Think ||
               action == nabo::Action::Wink);
    }
    animation.Start(nabo::Action::Listen, 10000);
    assert(animation.Frame(10000) == 5);
    assert(animation.Frame(11799) == 5);
    assert(animation.Frame(11800) == -1);
    animation.Start(nabo::Action::Music, 12000);
    assert(animation.Frame(12000) == 7);
    animation.Stop();
    assert(animation.Frame(12000) == -1);
    animation.Start(nabo::Action::Comfort, 13000);
    assert(animation.Frame(13000) == 6);
    animation.Start(nabo::Action::Happy, 13100);
    assert(animation.Frame(13100) == 0);
}
