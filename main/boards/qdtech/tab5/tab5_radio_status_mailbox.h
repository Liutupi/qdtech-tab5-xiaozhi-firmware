#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace tab5_radio_status {

// One owning status snapshot. The fixed buffers keep a burst of player updates
// from allocating or growing a queue on the MP3 decoding task.
struct Snapshot {
    std::array<char, 768> station{};
    std::array<char, 32> state{};
    std::array<char, 128> meta{};
    // A new HTTP stream may pass through Connecting/Buffering before the UI
    // tick. Keep the open event even when those statuses are coalesced.
    uint32_t stream_open_sequence = 0;
};

class LatestMailbox {
public:
    void Post(const char* station, const char* state, const char* meta) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state && std::strcmp(state, "Connecting") == 0)
            ++stream_open_sequence_;
        Copy(station, pending_.station);
        Copy(state, pending_.state);
        Copy(meta, pending_.meta);
        pending_.stream_open_sequence = stream_open_sequence_;
        pending_valid_ = true;
    }

    bool Take(Snapshot& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_valid_)
            return false;
        out = pending_;
        pending_valid_ = false;
        return true;
    }

private:
    template <std::size_t N>
    static void Copy(const char* text, std::array<char, N>& target) {
        target[0] = '\0';
        if (!text)
            return;
        const std::size_t length = strnlen(text, N - 1);
        std::size_t used = length;
        // When truncated, do not leave a partial UTF-8 codepoint on screen.
        if (length == N - 1 && text[length] != '\0') {
            while (used > 0 && (static_cast<unsigned char>(text[used]) & 0xc0) == 0x80)
                --used;
        }
        std::memcpy(target.data(), text, used);
        target[used] = '\0';
    }

    std::mutex mutex_;
    Snapshot pending_;
    uint32_t stream_open_sequence_ = 0;
    bool pending_valid_ = false;
};

}  // namespace tab5_radio_status
