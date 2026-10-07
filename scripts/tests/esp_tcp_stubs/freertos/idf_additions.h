#pragma once
#include <esp_heap_caps.h>
#include "task.h"

inline BaseType_t xTaskCreateWithCaps(TaskFunction_t function, const char*, uint32_t bytes,
                                      void* argument, UBaseType_t, TaskHandle_t* handle,
                                      UBaseType_t caps) {
    if (bytes != 4096 || caps != (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))
        std::abort();
    if (fake_fail_caps_stack.load())
        return 0;
    auto result = FakeCreateTask(function, argument, handle, true);
    if (result == pdPASS)
        fake_caps_created.fetch_add(1);
    return result;
}
inline void vTaskDeleteWithCaps(TaskHandle_t handle) {
    if (!handle || handle == xTaskGetCurrentTaskHandle() || !handle->caps)
        std::abort();
    std::unique_lock<std::mutex> lock(handle->mutex);
    handle->deleted = true;
    handle->cv.notify_all();
    handle->cv.wait(lock, [&]() { return handle->exited; });
    fake_caps_deleted.fetch_add(1);
}
