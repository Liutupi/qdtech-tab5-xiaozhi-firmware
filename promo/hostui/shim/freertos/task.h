#pragma once
#include "FreeRTOS.h"
#include <unistd.h>
#ifdef __cplusplus
#include <thread>
// Host: tasks are real threads; delays are scaled down because UI time is virtual.
static inline BaseType_t xTaskCreate(void (*fn)(void*), const char*, uint32_t, void* arg, int, TaskHandle_t*) {
    std::thread(fn, arg).detach(); return pdPASS; }
static inline BaseType_t xTaskCreatePinnedToCore(void (*fn)(void*), const char* n, uint32_t s, void* a, int p, TaskHandle_t* h, int) {
    return xTaskCreate(fn, n, s, a, p, h); }
#endif
static inline void vTaskDelay(TickType_t t) { usleep(t * 200); }
static inline void vTaskDelete(TaskHandle_t) {}
static inline unsigned uxTaskGetStackHighWaterMark(TaskHandle_t) { return 4096; }
