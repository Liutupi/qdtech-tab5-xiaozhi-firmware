#pragma once

#include <cstdint>

using BaseType_t = int;
using UBaseType_t = unsigned;
using TickType_t = uint32_t;

constexpr BaseType_t pdFALSE = 0;
constexpr BaseType_t pdPASS = 1;
constexpr unsigned configTICK_RATE_HZ = 1000;
constexpr uint32_t BIT0 = 1u << 0;
constexpr uint32_t BIT1 = 1u << 1;
constexpr uint32_t portTICK_PERIOD_MS = 1;
#define pdMS_TO_TICKS(ms) (ms)
