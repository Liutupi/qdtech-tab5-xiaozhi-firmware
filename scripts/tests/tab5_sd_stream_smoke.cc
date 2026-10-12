#include <cassert>
#include <cstdio>
#include "nabo_scene_stream.h"

using namespace nabo_sd;

int main() {
    StreamRecovery idle, sleep;
    uint64_t now = 0;
    for (unsigned i = 0; i < 32; ++i) {
        assert(idle.Ready(now));
        idle.Result(Error::Timeout, now);
        assert(!idle.Failed() && !idle.Ready(now));
        assert(sleep.Ready(now));  // Slow idle must not poison the sleep clip.
        now += 800;
    }
    assert(idle.Ready(now));
    idle.Result(Error::Ok, now);
    assert(idle.Ready(now));
    idle.Result(Error::Timeout, now);
    assert(!idle.Ready(now + 24) && idle.Ready(now + 25));
    idle.Result(Error::Cancelled, now);
    assert(!idle.Failed());
    sleep.Result(Error::Crc, now);
    assert(sleep.Failed() && !sleep.Ready(now + 100000));
    StreamRecovery payload;
    payload.Result(Error::Crc, now, true);
    assert(!payload.Failed() && payload.Ready(now + 25));

    FrameContinuity video;
    video.Begin(0);
    video.Presented(100);
    video.Begin(1320);             // Transition -> sleep while the old frame stays held.
    assert(!video.Expired(1920));  // 600ms SD latency used to flash a static face.
    video.Presented(1920);
    assert(!video.Expired(3920) && video.Expired(3921));
    video.Begin(4000);
    assert(!video.Expired(5000) && video.Expired(5001));
    std::puts(
        "PASS repeated SD latency recovers, per-clip backoff resets, corrupt assets stop, "
        "video survives delayed sleep handoff with bounded fallback");
}
