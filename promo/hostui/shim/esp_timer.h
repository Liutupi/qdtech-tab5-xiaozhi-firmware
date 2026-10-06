#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int64_t host_time_us(void);
static inline int64_t esp_timer_get_time(void) { return host_time_us(); }
typedef void* esp_timer_handle_t;
#ifdef __cplusplus
}
#endif
