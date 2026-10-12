#include "sdkconfig.h"

#if CONFIG_QDTECH_TAB5_NATIVE_UI && CONFIG_LV_USE_CUSTOM_MALLOC && CONFIG_SPIRAM
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <lvgl.h>

namespace {
constexpr uint32_t kCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

void CheckAllocation(void* ptr, size_t size) {
    if (!ptr && size)
        ESP_LOGE("Tab5UiMemory", "PSRAM allocation failed: bytes=%u free=%u largest=%u",
                 unsigned(size), unsigned(heap_caps_get_free_size(kCaps)),
                 unsigned(heap_caps_get_largest_free_block(kCaps)));
}
}  // namespace

// Native pages contain thousands of small objects, styles and glyphs. The
// default malloc policy puts each sub-1KB allocation in internal SRAM, starving
// FreeRTOS and networking when a page is first opened. LVGL does not need that
// capability; keep both small objects and large images in PSRAM. Never fall back
// to internal RAM when PSRAM is full, including when an allocation shrinks.
void lv_mem_init() {}
void lv_mem_deinit() {}
lv_mem_pool_t lv_mem_add_pool(void*, size_t) { return nullptr; }
void lv_mem_remove_pool(lv_mem_pool_t) {}

void* lv_malloc_core(size_t size) {
    void* ptr = heap_caps_malloc(size, kCaps);
    CheckAllocation(ptr, size);
    return ptr;
}

void* lv_realloc_core(void* ptr, size_t size) {
    void* next = heap_caps_realloc(ptr, size, kCaps);
    CheckAllocation(next, size);
    return next;
}

void lv_free_core(void* ptr) { heap_caps_free(ptr); }

// This allocator shares the ESP-IDF PSRAM heap with other components. Like the
// CLIB backend, it has no private LVGL pool to report or test.
void lv_mem_monitor_core(lv_mem_monitor_t*) {}
lv_result_t lv_mem_test_core() { return LV_RESULT_OK; }
#endif
