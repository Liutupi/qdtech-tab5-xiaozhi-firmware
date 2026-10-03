#include "tab5_frame_profile.h"

#include <cassert>
#include <cstdint>

int main() {
    using namespace tab5_frame;

    CauseGate gate;
    gate.MarkOnce(kPoseExit, 100);
    gate.MarkOnce(kDailyPage, 120);
    const uint8_t combined = gate.BeginFrame(130);
    assert(combined == (kPoseExit | kDailyPage));
    assert(gate.BeginFrame(140) == 0);  // One-shot marks cannot leak into later frames.

    gate.MarkPoseEntry(5, 200);
    assert(gate.BeginFrame(210) == kPoseEntry);
    assert(gate.PoseEntryAction() == 5);
    assert(gate.BeginFrame(211) == 0);
    assert(gate.PoseEntryAction() == kNoPoseAction);
    gate.MarkPoseEntry(6, 300);
    assert(gate.BeginFrame(801) == 0);  // Stale entry action cannot label another frame.
    assert(gate.PoseEntryAction() == kNoPoseAction);

    gate.MarkOnce(kSleepSwitch, 1000);
    assert(gate.BeginFrame(1501) == 0);  // A render delayed too long is unmarked.
    gate.MarkOnce(kSleepSwitch, UINT32_MAX - 10);
    assert(gate.BeginFrame(5) == kSleepSwitch);  // Tick rollover is harmless.

    gate.StartClockFlip(2000);
    assert(gate.BeginFrame(2000) == kClockFlip);
    assert(gate.BeginFrame(2220) == kClockFlip);
    assert(gate.BeginFrame(2221) == 0);

    Summary summary;
    summary.Observe(combined, 50000, 123456);
    summary.Observe(kPoseExit, 30000, 1000);
    summary.Observe(0, 60000, 700);
    assert(summary.Get(0).frames == 2);
    assert(summary.Get(0).slow40 == 1);
    assert(summary.Get(0).max_us == 50000);
    assert(summary.Get(0).pixels_at_max == 123456);
    assert(summary.Get(2).frames == 1 && summary.Get(2).slow40 == 1);
    assert(summary.Get(kUnmarked).frames == 1 && summary.Get(kUnmarked).slow40 == 1);
    assert(summary.multi_slow40() == 1);
    summary.Observe(kPoseEntry, 70000, 242172, 5);
    summary.Observe(kPoseEntry, 60000, 242382, 6);
    assert(summary.Get(kPoseEntryIndex).frames == 2);
    assert(summary.Get(kPoseEntryIndex).slow40 == 2);
    assert(summary.Get(kPoseEntryIndex).pixels_at_max == 242172);
    assert(summary.PoseEntryActionAtMax() == 5);
    summary.Reset();
    assert(summary.Get(0).frames == 0 && summary.multi_slow40() == 0);
    assert(summary.Get(kPoseEntryIndex).frames == 0);
    assert(summary.PoseEntryActionAtMax() == kNoPoseAction);
}
