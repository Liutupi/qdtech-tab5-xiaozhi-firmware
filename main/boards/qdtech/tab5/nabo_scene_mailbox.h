#pragma once
#include "nabo_framepack.h"

namespace nabo_sd {
// One UI producer + one reader. A newer frame can replace queued work, but only
// an epoch/clip change cancels an in-flight read. Slow storage skips frames.
class SceneMailbox {
    enum { Free, Reading, Ready, Held };
    struct Slot {
        std::atomic<int> state{Free};
        uint8_t* base = nullptr;
        uint8_t* pixels = nullptr;
        uint32_t token = 0;
        uint32_t serial = 0;
    };

public:
    struct PumpTrace {
        FrameTrace frame;
        uint32_t claim_us = 0, publish_us = 0, compose_us = 0;
        bool attempted = false, published = false, busy = false;
        void Reset() {
            const auto clock = frame.clock_us;
            *this = {};
            frame.clock_us = clock;
        }
    };
    struct Lease {
        int slot = -1;
        const uint8_t* pixels = nullptr;
        uint32_t token = 0;
    };
    // align > 1: each slot holds capacity bytes starting at an align-aligned base.
    // A frame is placed at base + (file offset % align), so every whole sector the
    // filesystem reads directly lands on an align-aligned address (DMA-capable).
    // c: optional third buffer so the reader never waits for the renderer to
    // release a slot (one shown, one ready, one being filled).
    SceneMailbox(uint8_t* a, uint8_t* b, size_t capacity, size_t align = 1, uint8_t* c = nullptr)
        : capacity_(capacity), align_(align ? align : 1) {
        slots_[0].base = slots_[0].pixels = a;
        slots_[1].base = slots_[1].pixels = b;
        slots_[2].base = slots_[2].pixels = c;
    }
    static constexpr int kSlots = 3;
    void Request(unsigned clip, unsigned frame, uint32_t generation) {
        if (clip >= 4 || frame >= 256) {
            CancelAll();
            return;
        }
        const uint32_t epoch = ((generation & 0x1fffffu) << 3) | (clip + 1);
        epoch_.store(epoch, std::memory_order_release);
        request_.store((epoch << 8) | frame, std::memory_order_release);
    }
    void CancelAll() {
        epoch_.store(0, std::memory_order_release);
        request_.store(0, std::memory_order_release);
    }
    // Optional worker-side post-process: frames are read into one staging buffer
    // (align-placed for DMA) and composed into the slot, which then holds the
    // composer's output format instead of the raw frame.
    using Compose = void (*)(void* context, const uint8_t* raw, uint8_t* out);
    void SetComposer(uint8_t* staging, size_t staging_capacity, Compose compose, void* context) {
        staging_ = staging;
        staging_capacity_ = staging_capacity;
        compose_ = compose;
        compose_context_ = context;
    }
    uint32_t Requested() const { return request_.load(std::memory_order_acquire); }
    static unsigned Clip(uint32_t token) { return ((token >> 8) & 7) - 1; }
    Error Pump(Pack& pack, uint32_t token, PumpTrace* trace = nullptr) {
        if (trace)
            trace->Reset();
        if (!token || token == completed_ || (token >> 8) != epoch_.load())
            return Error::Cancelled;
        const uint64_t claim_started = trace ? trace->frame.Now() : 0;
        for (auto& s : slots_) {
            if (!s.base)
                continue;
            int expected = Free;
            if (!s.state.compare_exchange_strong(expected, Reading))
                continue;
            if (trace) {
                trace->attempted = true;
                trace->claim_us = uint32_t(trace->frame.Now() - claim_started);
            }
            const size_t pad = pack.At(token & 255).offset % align_;
            uint8_t* const base = compose_ ? staging_ : s.base;
            const size_t room = compose_ ? staging_capacity_ : capacity_;
            uint8_t* const target = base + pad;
            const Error e = pack.Frame(token & 255, target, room > pad ? room - pad : 0,
                                       {&epoch_, token >> 8}, trace ? &trace->frame : nullptr);
            if (e != Error::Ok || (token >> 8) != epoch_.load()) {
                s.state.store(Free, std::memory_order_release);
                return e == Error::Ok ? Error::Cancelled : e;
            }
            if (compose_) {
                const uint64_t compose_started = trace ? trace->frame.Now() : 0;
                compose_(compose_context_, target, s.base);
                s.pixels = s.base;
                if (trace)
                    trace->compose_us = uint32_t(trace->frame.Now() - compose_started);
                if ((token >> 8) != epoch_.load()) {
                    s.state.store(Free, std::memory_order_release);
                    return Error::Cancelled;
                }
            } else {
                s.pixels = target;
            }
            const uint64_t publish_started = trace ? trace->frame.Now() : 0;
            s.token = token;
            s.serial = ++serial_;
            s.state.store(Ready, std::memory_order_release);
            completed_ = token;
            if (trace) {
                trace->published = true;
                trace->publish_us = uint32_t(trace->frame.Now() - publish_started);
            }
            return Error::Ok;
        }
        if (trace) {
            trace->busy = true;
            trace->claim_us = uint32_t(trace->frame.Now() - claim_started);
        }
        return Error::Ok;
    }
    Lease TakeReady() {
        Lease newest;
        uint32_t serial = 0;
        for (unsigned i = 0; i < slots_.size(); ++i) {
            auto& s = slots_[i];
            int expected = Ready;
            if (!s.state.compare_exchange_strong(expected, Held))
                continue;
            if ((s.token >> 8) != epoch_.load()) {
                s.state.store(Free, std::memory_order_release);
                continue;
            }
            if (newest.slot < 0 || int32_t(s.serial - serial) > 0) {
                Release(newest);
                newest = {int(i), s.pixels, s.token};
                serial = s.serial;
            } else
                s.state.store(Free, std::memory_order_release);
        }
        return newest;
    }
    void Release(Lease l) {
        if (l.slot < 0 || l.slot >= int(slots_.size()))
            return;
        int expected = Held;
        slots_[l.slot].state.compare_exchange_strong(expected, Free);
    }

private:
    size_t capacity_, align_;
    uint8_t* staging_ = nullptr;
    size_t staging_capacity_ = 0;
    Compose compose_ = nullptr;
    void* compose_context_ = nullptr;
    std::array<Slot, kSlots> slots_;
    std::atomic<uint32_t> epoch_{0}, request_{0};
    uint32_t completed_ = 0;
    uint32_t serial_ = 0;
};
}  // namespace nabo_sd
