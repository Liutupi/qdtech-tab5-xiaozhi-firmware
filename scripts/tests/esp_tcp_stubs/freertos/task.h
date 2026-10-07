#pragma once

#include "FreeRTOS.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

struct FakeTask {
    bool caps = false;
    bool deleted = false;
    bool exited = false;
    std::mutex mutex;
    std::condition_variable cv;
};
struct FakeTaskDeleted {};
using TaskHandle_t = FakeTask*;
using TaskFunction_t = void (*)(void*);

inline std::array<FakeTask, 64> fake_tasks;
inline std::atomic_size_t fake_task_count{0};
inline std::atomic_int fake_delay_calls{0};
inline std::atomic_bool fake_fail_internal_tls_stack{false};
inline std::atomic_bool fake_fail_caps_stack{false};
inline std::atomic_int fake_caps_created{0};
inline std::atomic_int fake_caps_deleted{0};
inline thread_local FakeTask fake_current_task;
inline thread_local TaskHandle_t fake_current_task_handle = &fake_current_task;

inline TaskHandle_t xTaskGetCurrentTaskHandle() { return fake_current_task_handle; }
inline TickType_t xTaskGetTickCount() {
    return static_cast<TickType_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());
}
inline void vTaskDelete(TaskHandle_t) {
    if (fake_current_task_handle->caps)
        std::abort();
}
inline void vTaskSuspend(TaskHandle_t handle) {
    if (handle != nullptr)
        std::abort();
    auto task = fake_current_task_handle;
    std::unique_lock<std::mutex> lock(task->mutex);
    task->cv.wait(lock, [&]() { return task->deleted; });
#if defined(__cpp_exceptions)
    throw FakeTaskDeleted{};
#else
    std::abort();
#endif
}
inline void vTaskDelay(TickType_t ticks) {
    fake_delay_calls.fetch_add(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
}

inline BaseType_t FakeCreateTask(TaskFunction_t function, void* argument, TaskHandle_t* handle,
                                 bool caps) {
    const auto index = fake_task_count.fetch_add(1);
    if (index >= fake_tasks.size())
        return 0;
    *handle = &fake_tasks[index];
    const auto task = *handle;
    task->caps = caps;
    std::thread([=]() {
        fake_current_task_handle = task;
#if defined(__cpp_exceptions)
        try {
            function(argument);
        } catch (const FakeTaskDeleted&) {
        }
#else
        function(argument);
#endif
        std::lock_guard<std::mutex> lock(task->mutex);
        task->exited = true;
        task->cv.notify_all();
    }).detach();
    return pdPASS;
}

inline BaseType_t xTaskCreate(TaskFunction_t function, const char* name, uint32_t, void* argument,
                              UBaseType_t, TaskHandle_t* handle) {
    if (std::strcmp(name, "ssl_receive") == 0 && fake_fail_internal_tls_stack.load())
        return 0;
    return FakeCreateTask(function, argument, handle, false);
}
