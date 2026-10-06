#pragma once
static inline bool lvgl_port_lock(int) { return true; }
static inline void lvgl_port_unlock(void) {}
