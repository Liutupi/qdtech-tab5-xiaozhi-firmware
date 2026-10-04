#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Host-tested core only. No ESP/LVGL integration and no allocation in this file.
namespace nabo_sd {
constexpr size_t kChunk = 8192, kMaxEntries = 256;
enum class Error { Ok, Io, Format, Bounds, Crc, Timeout, Cancelled, Capacity };
inline uint16_t U16(const uint8_t* p) { return p[0] | uint16_t(p[1]) << 8; }
inline uint32_t U32(const uint8_t* p) { return U16(p) | uint32_t(U16(p + 2)) << 16; }
inline uint32_t CrcStep(uint32_t c, const uint8_t* p, size_t n) {
    while (n--) {
        c ^= *p++;
        for (unsigned b = 0; b < 8; ++b)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return c;
}
inline uint32_t Crc(const uint8_t* p, size_t n) { return ~CrcStep(~0u, p, n); }

struct Source {
    virtual ~Source() = default;
    virtual uint64_t Size() const = 0;
    virtual size_t ReadAt(uint32_t offset, uint8_t* out, size_t bytes) = 0;
    virtual uint64_t NowMs() const = 0;
};
struct Cancel {
    const std::atomic<uint32_t>* request = nullptr;
    uint32_t token = 0;
    bool Changed() const { return request && request->load(std::memory_order_acquire) != token; }
};
struct Entry {
    uint32_t at_ms, offset, bytes, crc;
    uint16_t x, y, width, height;
};

class Pack {
public:
    Error Open(Source& source, size_t frame_capacity, uint32_t timeout_ms = 80,
               bool allow_scene = false) {
        source_ = &source;
        valid_ = false;
        timeout_ms_ = timeout_ms;
        uint8_t h[64];
        Error e = Read(0, h, sizeof(h), {});
        if (e != Error::Ok)
            return e;
        const bool supported = (U16(h + 8) == 1 && U32(h + 40) == 1) ||
                               (allow_scene && U16(h + 8) == 2 && U32(h + 40) == 2);
        if (std::memcmp(h, "NABOSD1\0", 8) || !supported || U16(h + 10) != 64 ||
            Crc(h, 60) != U32(h + 60))
            return Error::Format;
        width_ = U16(h + 12);
        height_ = U16(h + 14);
        const uint16_t max_w = U16(h + 16), max_h = U16(h + 18);
        count_ = U32(h + 20);
        duration_ = U32(h + 24);
        const uint32_t index_offset = U32(h + 28), data_offset = U32(h + 32);
        max_payload_ = U32(h + 36);
        if (!width_ || !height_ || width_ > 512 || height_ > 558 || !max_w || !max_h ||
            max_w > width_ || max_h > height_ || !count_ || count_ > kMaxEntries ||
            duration_ < 40 || duration_ > 30000 || index_offset != 64 ||
            data_offset != 64 + count_ * 32 || source.Size() != U32(h + 44) || U32(h + 52) ||
            U32(h + 56) || max_payload_ != uint32_t(max_w) * max_h * 3)
            return Error::Bounds;
        if (max_payload_ > frame_capacity)
            return Error::Capacity;
        e = Read(index_offset, index_.data(), count_ * 32, {});
        if (e != Error::Ok)
            return e;
        if (Crc(index_.data(), count_ * 32) != U32(h + 48))
            return Error::Crc;
        uint32_t previous = 0;
        for (uint32_t i = 0; i < count_; ++i) {
            const uint8_t* raw = index_.data() + i * 32;
            const Entry entry = At(i);
            if ((!i && entry.at_ms) || (i && entry.at_ms <= previous) || entry.at_ms >= duration_ ||
                !entry.width || !entry.height || entry.width > max_w || entry.height > max_h ||
                uint32_t(entry.x) + entry.width > width_ ||
                uint32_t(entry.y) + entry.height > height_ ||
                entry.bytes != uint32_t(entry.width) * entry.height * 3 ||
                entry.bytes > frame_capacity || entry.offset < data_offset ||
                uint64_t(entry.offset) + entry.bytes > source.Size() || raw[24] != 1 ||
                raw[25] != 1 || U16(raw + 26) || U32(raw + 28))
                return Error::Bounds;
            previous = entry.at_ms;
        }
        valid_ = true;
        return Error::Ok;
    }
    bool Valid() const { return valid_; }
    uint32_t Duration() const { return duration_; }
    uint32_t Count() const { return count_; }
    uint32_t MaxPayload() const { return max_payload_; }
    Entry At(uint32_t i) const {
        if (i >= count_ || i >= kMaxEntries)
            return {};
        const uint8_t* p = index_.data() + i * 32;
        return {U32(p),      U32(p + 4),  U32(p + 8),  U32(p + 12),
                U16(p + 16), U16(p + 18), U16(p + 20), U16(p + 22)};
    }
    uint32_t Due(uint64_t elapsed_ms) const {
        if (!valid_)
            return 0;
        const uint32_t phase = elapsed_ms % duration_;
        uint32_t i = 0;
        while (i + 1 < count_ && At(i + 1).at_ms <= phase)
            ++i;
        return i;
    }
    Error Frame(uint32_t i, uint8_t* out, size_t capacity, Cancel cancel = {}) {
        if (!valid_ || i >= count_)
            return Error::Bounds;
        const Entry e = At(i);
        if (!out || e.bytes > capacity)
            return Error::Capacity;
        const Error result = Read(e.offset, out, e.bytes, cancel);
        if (result != Error::Ok)
            return result;
        const uint64_t started = source_->NowMs();
        // CRC is chunked too, so a state change cannot require a full-frame CRC first.
        uint32_t crc = ~0u;
        for (size_t off = 0; off < e.bytes; off += kChunk) {
            if (cancel.Changed())
                return Error::Cancelled;
            crc = CrcStep(crc, out + off, std::min(kChunk, size_t(e.bytes) - off));
            if (source_->NowMs() - started > timeout_ms_)
                return Error::Timeout;
        }
        if (cancel.Changed())
            return Error::Cancelled;
        return ~crc == e.crc ? Error::Ok : Error::Crc;
    }

private:
    Error Read(uint32_t offset, uint8_t* out, size_t bytes, Cancel cancel) {
        const uint64_t started = source_->NowMs();
        size_t done = 0;
        while (done < bytes) {
            if (cancel.Changed())
                return Error::Cancelled;
            const size_t chunk = std::min(kChunk, bytes - done);
            const size_t got = source_->ReadAt(offset + done, out + done, chunk);
            // A synchronous filesystem call itself is not preemptible here.
            // This method belongs on a worker, never the UI/audio thread.
            if (cancel.Changed())
                return Error::Cancelled;
            if (source_->NowMs() - started > timeout_ms_)
                return Error::Timeout;
            if (got != chunk)
                return Error::Io;
            done += got;
        }
        return Error::Ok;
    }
    Source* source_ = nullptr;
    std::array<uint8_t, kMaxEntries * 32> index_{};
    uint32_t count_ = 0, duration_ = 0, max_payload_ = 0, timeout_ms_ = 80;
    uint16_t width_ = 0, height_ = 0;
    bool valid_ = false;
};

struct Gates {
    bool idle = true, audio_pending = false, voice_detected = false, sleeping = false;
    bool preview = false, app_visible = false, gesture = false, music_playing = false;
    bool Allow() const {
        return idle && !audio_pending && !voice_detected && !sleeping && !preview && !app_visible &&
               !gesture && !music_playing;
    }
};

// Single UI producer/consumer + one IO worker. Call Pack::Open before constructing.
// Buffers are caller-owned, allocated once; Hold/Release prevents use-after-swap.
class Player {
    enum SlotState { Free, Reading, Ready, Held };
    struct Slot {
        std::atomic<int> state{Free};
        uint8_t* pixels = nullptr;
        uint32_t token = 0;
        Entry entry{};
    };

public:
    struct Lease {
        int slot = -1;
        const uint8_t* pixels = nullptr;
        Entry entry{};
    };
    Player(Pack& pack, uint8_t* a, uint8_t* b, size_t capacity) : pack_(pack), capacity_(capacity) {
        slots_[0].pixels = a;
        slots_[1].pixels = b;
        if (!pack.Valid() || !a || !b || a == b || capacity < pack.MaxPayload())
            fault_.store(Error::Capacity);
    }
    // UI only: no file IO, allocation, mutex, or waiting. False means show static now.
    bool Update(uint64_t now_ms, const Gates& gates) {
        if (!gates.Allow() || fault_.load() != Error::Ok) {
            active_ = false;
            request_.store(0, std::memory_order_release);
            return false;
        }
        if (!active_) {
            active_ = true;
            start_ = now_ms;
            generation_ = (generation_ + 1) & 0xffffffu;
            if (!generation_)
                generation_ = 1;
        }
        const uint32_t token = (generation_ << 8) | pack_.Due(now_ms - start_);
        const uint32_t old_request = request_.load();
        if (old_request != token) {
            if (!old_request || old_request == shown_token_)
                pending_since_ = now_ms;
            request_.store(token, std::memory_order_release);
        }
        if (shown_token_ != token && now_ms - pending_since_ > 160) {
            fault_.store(Error::Timeout);
            request_.store(0, std::memory_order_release);
            active_ = false;
            return false;
        }
        return true;
    }
    // Worker only. No queues: service the latest requested timeline position.
    Error Pump() {
        const uint32_t token = request_.load(std::memory_order_acquire);
        if (!token || token == completed_token_)
            return Error::Ok;
        for (auto& slot : slots_) {
            int expected = Free;
            if (!slot.state.compare_exchange_strong(expected, Reading))
                continue;
            const Error e = pack_.Frame(token & 255, slot.pixels, capacity_, {&request_, token});
            if (e != Error::Ok || request_.load() != token) {
                slot.state.store(Free, std::memory_order_release);
                if (e != Error::Ok && e != Error::Cancelled)
                    fault_.store(e);
                return e == Error::Ok ? Error::Cancelled : e;
            }
            slot.token = token;
            slot.entry = pack_.At(token & 255);
            slot.state.store(Ready, std::memory_order_release);
            completed_token_ = token;
            return Error::Ok;
        }
        return Error::Ok;  // Renderer still holds both slots; never overwrite them.
    }
    // UI only; drop stale completions. The caller releases the PREVIOUS lease only
    // after LVGL has stopped referencing it (render/flush completion, as applicable).
    Lease TakeReady() {
        for (size_t i = 0; i < slots_.size(); ++i) {
            auto& s = slots_[i];
            int expected = Ready;
            if (!s.state.compare_exchange_strong(expected, Held))
                continue;
            if (s.token != request_.load(std::memory_order_acquire) || fault_.load() != Error::Ok) {
                s.state.store(Free, std::memory_order_release);
                continue;
            }
            shown_token_ = s.token;
            return {int(i), s.pixels, s.entry};
        }
        return {};
    }
    void Release(Lease lease) {
        if (lease.slot >= 0 && lease.slot < 2) {
            int held = Held;
            slots_[lease.slot].state.compare_exchange_strong(held, Free);
        }
    }
    Error Fault() const { return fault_.load(); }
    uint32_t RequestForTest() const { return request_.load(); }

private:
    static_assert(std::atomic<uint32_t>::is_always_lock_free, "UI request needs lock-free atomics");
    Pack& pack_;
    const size_t capacity_;
    std::array<Slot, 2> slots_;
    std::atomic<uint32_t> request_{0};
    std::atomic<Error> fault_{Error::Ok};
    uint32_t generation_ = 0, shown_token_ = 0, completed_token_ = 0;
    uint64_t start_ = 0, pending_since_ = 0;
    bool active_ = false;
};
}  // namespace nabo_sd
