#pragma once
#include <stdint.h>
typedef void* TaskHandle_t; typedef void* QueueHandle_t; typedef int BaseType_t; typedef uint32_t TickType_t;
#define pdPASS 1
#define pdFAIL 0
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(x) (x)
