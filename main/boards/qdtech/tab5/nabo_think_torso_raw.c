#include "nabo_think_torso_raw.h"

#include <stdint.h>

enum {
    kThinkTorsoWidth = 262,
    kThinkTorsoHeight = 462,
    kThinkTorsoBytes = kThinkTorsoWidth * kThinkTorsoHeight * 3,
};

// The experiment-only assembly places the RGB565 and alpha planes in flash.
extern const uint8_t think_torso_raw_start[] asm("_binary_think_torso_262x462_rgb565a8_bin_start");
extern const uint8_t think_torso_raw_end[] asm("_binary_think_torso_262x462_rgb565a8_bin_end");

const lv_image_dsc_t nabo_think_torso_raw = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_RGB565A8,
    .header.stride = kThinkTorsoWidth * 2,
    .header.w = kThinkTorsoWidth,
    .header.h = kThinkTorsoHeight,
    .data_size = kThinkTorsoBytes,
    .data = think_torso_raw_start,
};

bool nabo_think_torso_raw_available(void) {
    const uintptr_t begin = (uintptr_t)think_torso_raw_start;
    const uintptr_t end = (uintptr_t)think_torso_raw_end;
    return (begin & 3u) == 0 && end >= begin && end - begin == kThinkTorsoBytes;
}
