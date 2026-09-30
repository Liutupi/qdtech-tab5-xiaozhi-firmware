#pragma once
// Internal-SRAM diagnostics for the Tab5.
//
// The v1.0.0 serial log shows internal SRAM dropping from ~354 KB (early boot) to ~54 KB after
// board init and to <7 KB once the network is up (min 1.7 KB).  These helpers tell us who holds it:
//   Tab5MemDiag::Stage("label")   one-line free/largest/min after a boot stage
//   Tab5MemDiag::ScheduleReport() one full task + heap dump ~45 s after boot (own short-lived task)
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include <cstdlib>

namespace Tab5MemDiag {

inline void Stage(const char* label) {
    ESP_LOGI("MemDiag", "%-24s internal free=%u largest=%u min=%u | dma free=%u | psram free=%u", label,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

inline void DumpTasks() {
    const UBaseType_t count = uxTaskGetNumberOfTasks();
    auto* status = static_cast<TaskStatus_t*>(
        heap_caps_calloc(count + 4, sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!status) {
        ESP_LOGW("MemDiag", "task dump: no memory");
        return;
    }
    const UBaseType_t got = uxTaskGetSystemState(status, count + 4, nullptr);
    ESP_LOGI("MemDiag", "---- %u tasks (stack location / never-used stack bytes) ----", (unsigned)got);
    for (UBaseType_t i = 0; i < got; ++i) {
        const auto& t = status[i];
        ESP_LOGI("MemDiag", "task %-16s prio=%2u stack=%s hwm=%u", t.pcTaskName, (unsigned)t.uxCurrentPriority,
                 esp_ptr_internal(t.pxStackBase) ? "INTERNAL" : "psram   ", (unsigned)t.usStackHighWaterMark);
    }
    heap_caps_free(status);
}

inline void Report() {
    Stage("report");
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    DumpTasks();
}

// Runs Report() once, `delay_ms` after the call, from a small PSRAM-stack task.
inline void ScheduleReport(uint32_t delay_ms = 45000) {
    static uint32_t s_delay;
    s_delay = delay_ms;
    xTaskCreateWithCaps(
        [](void*) {
            vTaskDelay(pdMS_TO_TICKS(s_delay));
            Report();
            vTaskDelete(nullptr);  // one-off: the 6 KB PSRAM stack is intentionally left to the OS
        },
        "memdiag", 6144, nullptr, 3, nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

}  // namespace Tab5MemDiag
