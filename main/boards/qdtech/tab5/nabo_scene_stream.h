#pragma once

#include "nabo_framepack.h"

namespace nabo_sd {
// Slow SD/Wi-Fi transactions and individual bad payloads are recoverable.
// Corrupt headers/indexes and incompatible assets still stop immediately.
class StreamRecovery {
public:
    bool Ready(uint64_t now) const { return !failed_ && now >= retry_at_; }
    bool Failed() const { return failed_; }
    void Result(Error error, uint64_t now, bool payload = false) {
        if (error == Error::Ok) {
            timeouts_ = 0;
            retry_at_ = 0;
        } else if (error == Error::Timeout || (error == Error::Crc && payload)) {
            timeouts_ = std::min(timeouts_ + 1, 6u);
            retry_at_ = now + (25u << (timeouts_ - 1));
        } else if (error != Error::Cancelled) {
            failed_ = true;
        }
    }

private:
    uint64_t retry_at_ = 0;
    unsigned timeouts_ = 0;
    bool failed_ = false;
};

// A clip change gets its own first-frame budget while the previous video stays
// visible. Do not replace it with a neutral face just because storage is slower
// than the 40ms source cadence.
class FrameContinuity {
public:
    static constexpr uint64_t kFirstFrameMs = 1000, kHoldVideoMs = 2000;
    void Begin(uint64_t now) {
        since_ = now;
        received_ = false;
    }
    void Presented(uint64_t now) {
        since_ = now;
        received_ = true;
    }
    bool Expired(uint64_t now) const {
        return now - since_ > (received_ ? kHoldVideoMs : kFirstFrameMs);
    }

private:
    uint64_t since_ = 0;
    bool received_ = false;
};
}  // namespace nabo_sd
