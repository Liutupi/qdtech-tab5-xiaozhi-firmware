#pragma once

#include "FreeRTOS.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

using EventBits_t = uint32_t;

struct FakeEventGroup {
    std::mutex mutex;
    std::condition_variable cv;
    EventBits_t bits = 0;
};

using EventGroupHandle_t = FakeEventGroup*;
inline std::atomic_int fake_wait_calls{0};
inline std::atomic_int fake_deleted_groups{0};

inline EventGroupHandle_t xEventGroupCreate() { return new FakeEventGroup; }

inline void vEventGroupDelete(EventGroupHandle_t group) {
    fake_deleted_groups.fetch_add(1);
    delete group;
}

inline EventBits_t xEventGroupClearBits(EventGroupHandle_t group, EventBits_t bits) {
    std::lock_guard<std::mutex> lock(group->mutex);
    const auto old = group->bits;
    group->bits &= ~bits;
    return old;
}

inline EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits) {
    std::lock_guard<std::mutex> lock(group->mutex);
    group->bits |= bits;
    group->cv.notify_all();
    return group->bits;
}

inline EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                                       BaseType_t clear_on_exit, BaseType_t wait_all,
                                       TickType_t timeout_ms) {
    fake_wait_calls.fetch_add(1);
    std::unique_lock<std::mutex> lock(group->mutex);
    const auto ready = [&]() {
        return wait_all ? (group->bits & bits) == bits : (group->bits & bits) != 0;
    };
    group->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready);
    const auto result = group->bits;
    if (clear_on_exit) group->bits &= ~bits;
    return result;
}
