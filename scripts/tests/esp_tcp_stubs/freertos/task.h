#pragma once

#include "FreeRTOS.h"

#include <array>
#include <atomic>
#include <chrono>
#include <thread>

struct FakeTask {};
using TaskHandle_t = FakeTask*;
using TaskFunction_t = void (*)(void*);

inline std::array<FakeTask, 64> fake_tasks;
inline std::atomic_size_t fake_task_count{0};
inline std::atomic_int fake_delay_calls{0};
inline thread_local FakeTask fake_current_task;
inline thread_local TaskHandle_t fake_current_task_handle = &fake_current_task;

inline TaskHandle_t xTaskGetCurrentTaskHandle() { return fake_current_task_handle; }
inline TickType_t xTaskGetTickCount() {
    return static_cast<TickType_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline void vTaskDelete(TaskHandle_t) {}
inline void vTaskDelay(TickType_t ticks) {
    fake_delay_calls.fetch_add(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
}

inline BaseType_t xTaskCreate(TaskFunction_t function, const char*, uint32_t, void* argument,
                              UBaseType_t, TaskHandle_t* handle) {
    const auto index = fake_task_count.fetch_add(1);
    if (index >= fake_tasks.size()) return 0;
    *handle = &fake_tasks[index];
    const auto task = *handle;
    std::thread([=]() {
        fake_current_task_handle = task;
        function(argument);
    }).detach();
    return pdPASS;
}
