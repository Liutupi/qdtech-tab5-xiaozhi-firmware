#include "gwenesis_tab5.h"

#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

void* gwenesis_tab5_alloc(size_t size) {
    void* p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p)
        p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        ESP_LOGE("gwenesis", "out of memory for %u bytes", (unsigned)size);
        abort();
    }
    return p;
}
