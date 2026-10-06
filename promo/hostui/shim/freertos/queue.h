#pragma once
#include "FreeRTOS.h"
#include <cstring>
#include <deque>
// Host queue of pointer-sized items; the harness drains it on its main loop.
inline std::deque<void*>& HostQueue() { static std::deque<void*> q; return q; }
static inline QueueHandle_t xQueueCreate(int, int) { static int q; return &q; }
static inline BaseType_t xQueueSend(QueueHandle_t, const void* item, TickType_t) {
    void* p; std::memcpy(&p, item, sizeof(p)); HostQueue().push_back(p); return pdTRUE; }
static inline BaseType_t xQueueReceive(QueueHandle_t, void*, TickType_t) { return pdFAIL; }
