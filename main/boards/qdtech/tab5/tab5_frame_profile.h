#pragma once

#include <cstdint>

namespace tab5_frame {

enum Cause : uint8_t {
    kPoseExit = 1u << 0,
    kSleepSwitch = 1u << 1,
    kDailyPage = 1u << 2,
    kClockFlip = 1u << 3,
    kPoseEntry = 1u << 4,
};

constexpr unsigned kCauseCount = 5;
constexpr unsigned kUnmarked = kCauseCount;
constexpr unsigned kClockFlipIndex = 3;
constexpr unsigned kPoseEntryIndex = 4;
constexpr uint8_t kNoPoseAction = UINT8_MAX;

// One-shot UI changes belong to the next rendered frame only. The clock flap
// changes for 180 ms, so its context stays active across a few render frames.
class CauseGate {
public:
    void MarkOnce(Cause cause, uint32_t now_ms) {
        for (unsigned i = 0; i < kCauseCount; ++i) {
            if (i == kClockFlipIndex)  // Clock flips have a separate timed gate.
                continue;
            if (cause == (1u << i)) {
                pending_ |= cause;
                marked_at_ms_[i] = now_ms;
                return;
            }
        }
    }

    void MarkPoseEntry(uint8_t action_id, uint32_t now_ms) {
        MarkOnce(kPoseEntry, now_ms);
        pending_pose_action_ = action_id;
    }

    void StartClockFlip(uint32_t now_ms) {
        clock_active_ = true;
        clock_started_ms_ = now_ms;
    }

    uint8_t BeginFrame(uint32_t now_ms) {
        uint8_t causes = 0;
        frame_pose_action_ = kNoPoseAction;
        for (unsigned i = 0; i < kCauseCount; ++i) {
            if (i == kClockFlipIndex)
                continue;
            const uint8_t bit = 1u << i;
            if ((pending_ & bit) && now_ms - marked_at_ms_[i] <= kOneShotLifetimeMs) {
                causes |= bit;
                if (bit == kPoseEntry)
                    frame_pose_action_ = pending_pose_action_;
            }
        }
        pending_ = 0;
        pending_pose_action_ = kNoPoseAction;
        if (clock_active_) {
            if (now_ms - clock_started_ms_ <= kClockLifetimeMs)
                causes |= kClockFlip;
            else
                clock_active_ = false;
        }
        return causes;
    }

    uint8_t PoseEntryAction() const { return frame_pose_action_; }

private:
    static constexpr uint32_t kOneShotLifetimeMs = 500;
    static constexpr uint32_t kClockLifetimeMs = 220;
    uint32_t marked_at_ms_[kCauseCount] = {};
    uint32_t clock_started_ms_ = 0;
    uint8_t pending_ = 0;
    uint8_t pending_pose_action_ = kNoPoseAction;
    uint8_t frame_pose_action_ = kNoPoseAction;
    bool clock_active_ = false;
};

struct Bucket {
    uint32_t frames = 0;
    uint32_t slow40 = 0;
    uint32_t max_us = 0;
    uint64_t pixels_at_max = 0;
};

// Categories indicate co-occurrence, not exclusive or proven causes. A frame
// with two marks counts in both buckets; the unmarked bucket gets only frames
// without marks. Pixel count belongs to the frame with each bucket's max time.
class Summary {
public:
    void Observe(uint8_t causes, uint32_t elapsed_us, uint64_t flush_pixels,
                 uint8_t pose_action = kNoPoseAction) {
        if (!causes) {
            Record(buckets_[kUnmarked], elapsed_us, flush_pixels);
            return;
        }
        if (elapsed_us > 40000 && (causes & (causes - 1)))
            ++multi_slow40_;
        for (unsigned i = 0; i < kCauseCount; ++i) {
            if (causes & (1u << i)) {
                if (i == kPoseEntryIndex && elapsed_us > buckets_[i].max_us)
                    pose_action_at_max_ = pose_action;
                Record(buckets_[i], elapsed_us, flush_pixels);
            }
        }
    }

    const Bucket& Get(unsigned index) const { return buckets_[index]; }
    uint8_t PoseEntryActionAtMax() const { return pose_action_at_max_; }
    uint32_t multi_slow40() const { return multi_slow40_; }
    void Reset() {
        for (auto& bucket : buckets_)
            bucket = {};
        pose_action_at_max_ = kNoPoseAction;
        multi_slow40_ = 0;
    }

private:
    static void Record(Bucket& bucket, uint32_t elapsed_us, uint64_t flush_pixels) {
        ++bucket.frames;
        bucket.slow40 += elapsed_us > 40000;
        if (elapsed_us > bucket.max_us) {
            bucket.max_us = elapsed_us;
            bucket.pixels_at_max = flush_pixels;
        }
    }

    Bucket buckets_[kCauseCount + 1] = {};
    uint8_t pose_action_at_max_ = kNoPoseAction;
    uint32_t multi_slow40_ = 0;
};

}  // namespace tab5_frame
