#pragma once

#include <cstdint>
#include <mutex>
#include <string_view>

// Audio-output time, not download time or wall time. Nothing advances during
// buffering. A new HTTP stream (including reconnect) starts from zero.
namespace tab5_playback {
struct Position {
    bool matches = false;
    uint32_t generation = 0;
    uint32_t milliseconds = 0;
};

class Clock {
public:
    void Begin(std::string_view url, uint32_t generation) {
        std::lock_guard<std::mutex> lock(mutex_);
        source_ = Key(url);
        generation_ = generation;
        microseconds_ = 0;
    }
    void Advance(int samples, int rate, uint32_t generation) {
        if (samples <= 0 || rate <= 0)
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation == generation_)
            microseconds_ += uint64_t(samples) * 1000000 / unsigned(rate);
    }
    Position Read(std::string_view url, uint32_t current_generation) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool matches = generation_ == current_generation && source_ == Key(url);
        return {matches, generation_, matches ? uint32_t(microseconds_ / 1000) : 0};
    }

private:
    static uint64_t Key(std::string_view url) {
        uint64_t value = 14695981039346656037ULL;
        for (unsigned char c : url)
            value = (value ^ c) * 1099511628211ULL;
        return value;
    }
    mutable std::mutex mutex_;
    uint64_t source_ = 0;
    uint32_t generation_ = 0;
    uint64_t microseconds_ = 0;
};
}  // namespace tab5_playback
