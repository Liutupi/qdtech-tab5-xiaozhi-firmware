#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace nabo_sd {
inline constexpr unsigned kClipBits = 6;
inline constexpr unsigned kClipCapacity = (1u << kClipBits) - 1;
using FailedClips = uint64_t;

inline constexpr FailedClips ClipBit(unsigned clip) {
    return clip < kClipCapacity ? FailedClips{1} << clip : 0;
}

// Failures only accumulate until reboot. Separate lock-free 32-bit words avoid
// introducing a 64-bit atomic lock/helper into the UI and SD reader tasks.
class ClipFailures {
public:
    void Mark(unsigned clip) {
        if (clip < kClipCapacity)
            words_[clip / 32].fetch_or(uint32_t{1} << (clip % 32), std::memory_order_release);
    }
    bool Contains(unsigned clip) const {
        return clip < kClipCapacity &&
               (words_[clip / 32].load(std::memory_order_acquire) & (uint32_t{1} << (clip % 32)));
    }
    FailedClips Snapshot() const {
        return FailedClips{words_[0].load(std::memory_order_acquire)} |
               (FailedClips{words_[1].load(std::memory_order_acquire)} << 32);
    }

private:
    std::array<std::atomic<uint32_t>, 2> words_{{0, 0}};
};
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(sizeof(ClipFailures) == 2 * sizeof(uint32_t));
static_assert(kClipCapacity < sizeof(FailedClips) * 8);
}  // namespace nabo_sd
