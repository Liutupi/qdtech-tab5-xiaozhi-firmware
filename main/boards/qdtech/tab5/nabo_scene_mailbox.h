#pragma once
#include "nabo_framepack.h"

namespace nabo_sd {
// One UI producer + one reader. A newer frame can replace queued work, but only
// an epoch/clip change cancels an in-flight read. Slow storage skips frames.
class SceneMailbox {
    enum { Free, Reading, Ready, Held };
    struct Slot {
        std::atomic<int> state{Free};
        uint8_t* pixels = nullptr;
        uint32_t token = 0;
        uint32_t serial = 0;
    };

public:
    struct PumpTrace {
        FrameTrace frame;
        uint32_t claim_us = 0, publish_us = 0;
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
    SceneMailbox(uint8_t* a, uint8_t* b, size_t capacity) : capacity_(capacity) {
        slots_[0].pixels = a;
        slots_[1].pixels = b;
    }
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
    uint32_t Requested() const { return request_.load(std::memory_order_acquire); }
    static unsigned Clip(uint32_t token) { return ((token >> 8) & 7) - 1; }
    Error Pump(Pack& pack, uint32_t token, PumpTrace* trace = nullptr) {
        if (trace)
            trace->Reset();
        if (!token || token == completed_ || (token >> 8) != epoch_.load())
            return Error::Cancelled;
        const uint64_t claim_started = trace ? trace->frame.Now() : 0;
        for (auto& s : slots_) {
            int expected = Free;
            if (!s.state.compare_exchange_strong(expected, Reading))
                continue;
            if (trace) {
                trace->attempted = true;
                trace->claim_us = uint32_t(trace->frame.Now() - claim_started);
            }
            const Error e = pack.Frame(token & 255, s.pixels, capacity_, {&epoch_, token >> 8},
                                       trace ? &trace->frame : nullptr);
            if (e != Error::Ok || (token >> 8) != epoch_.load()) {
                s.state.store(Free, std::memory_order_release);
                return e == Error::Ok ? Error::Cancelled : e;
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
        for (unsigned i = 0; i < 2; ++i) {
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
        if (l.slot < 0 || l.slot >= 2)
            return;
        int expected = Held;
        slots_[l.slot].state.compare_exchange_strong(expected, Free);
    }

private:
    size_t capacity_;
    std::array<Slot, 2> slots_;
    std::atomic<uint32_t> epoch_{0}, request_{0};
    uint32_t completed_ = 0;
    uint32_t serial_ = 0;
};
}  // namespace nabo_sd
