#include "nabo_portrait_matte.h"

#include <stdint.h>

enum {
    kPortraitWidth = 434,
    kPortraitHeight = 558,
    kPortraitBytes = kPortraitWidth * kPortraitHeight * 2,
};

// The experiment-only assembly places the RGB565 data at a 4-byte boundary.
extern const uint8_t portrait_matte_start[] asm("_binary_portrait_matte_434x558_rgb565_bin_start");
extern const uint8_t portrait_matte_end[] asm("_binary_portrait_matte_434x558_rgb565_bin_end");

const lv_image_dsc_t nabo_portrait_matte = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_RGB565,
    .header.stride = kPortraitWidth * 2,
    .header.w = kPortraitWidth,
    .header.h = kPortraitHeight,
    .data_size = kPortraitBytes,
    .data = portrait_matte_start,
};

bool nabo_portrait_matte_available(void) {
    const uintptr_t begin = (uintptr_t)portrait_matte_start;
    const uintptr_t end = (uintptr_t)portrait_matte_end;
    return (begin & 3u) == 0 && end >= begin && end - begin == kPortraitBytes;
}
