#pragma once
#include <atomic>
#include <cstdint>
#include "device_state.h"

// Observational UI state only: never starts/stops recording, playback or transport.
// A transcript may enter waiting only after speech in the current listening turn.
// Generation bits prevent a concurrent cancel/new utterance from publishing an old wait.
class ReplyWaitState {
public:
    void Reset() { Advance(0); }
    void Voice(bool speaking) {
        if (speaking)
            Advance(kSeenVoice | kVoice);
        else
            state_.fetch_and(~kVoice, std::memory_order_acq_rel);
    }
    bool SawVoice() const { return state_.load(std::memory_order_acquire) & kSeenVoice; }
    bool Recognized(uint32_t now, DeviceState state) {
        if (state != kDeviceStateListening && state != kDeviceStateIdle)
            return false;
        uint32_t observed = state_.load(std::memory_order_acquire);
        if (!(observed & kSeenVoice) || (observed & kVoice))
            return false;
        if (observed & kWaiting)
            return true;  // Repeated STT does not extend the deadline.
        since_.store(now, std::memory_order_relaxed);
        return state_.compare_exchange_strong(observed, observed | kWaiting,
                                              std::memory_order_release, std::memory_order_relaxed);
    }
    void SubmittedManual(uint32_t now, bool had_voice) {
        if (!had_voice)
            return;
        since_.store(now, std::memory_order_relaxed);
        Advance(kSeenVoice | kWaiting);
    }
    bool Waiting(uint32_t now, DeviceState state, bool playback_active = false) {
        if (playback_active) {
            Reset();
            return false;
        }
        uint32_t observed = state_.load(std::memory_order_acquire);
        if (!(observed & kWaiting) || (observed & kVoice))
            return false;
        if (uint32_t(now - since_.load(std::memory_order_relaxed)) >= kTimeoutMs) {
            state_.compare_exchange_strong(observed, Next(observed, 0), std::memory_order_acq_rel);
            return false;
        }
        return (state == kDeviceStateListening || state == kDeviceStateIdle) &&
               state_.load(std::memory_order_acquire) == observed;
    }
    static constexpr uint32_t kTimeoutMs = 60000;

private:
    static constexpr uint32_t kSeenVoice = 1, kVoice = 2, kWaiting = 4;
    static uint32_t Next(uint32_t previous, uint32_t flags) {
        return ((previous + 8) & ~7u) | flags;
    }
    void Advance(uint32_t flags) {
        uint32_t previous = state_.load(std::memory_order_relaxed);
        while (!state_.compare_exchange_weak(previous, Next(previous, flags),
                                             std::memory_order_acq_rel)) {
        }
    }
    // Use the SDK's 32-bit atomics. Xtensa's PSRAM workaround may report them
    // as not always lock-free even though the SDK provides the implementation.
    std::atomic<uint32_t> state_{0}, since_{0};
};
