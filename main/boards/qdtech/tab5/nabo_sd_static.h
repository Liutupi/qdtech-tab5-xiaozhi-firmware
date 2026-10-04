#pragma once
#include "lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const lv_image_dsc_t* image;
    int16_t x, y;
} nabo_sd_patch_t;
extern const lv_image_dsc_t nabo_sd_neutral;
extern const nabo_sd_patch_t nabo_sd_eyes[2], nabo_sd_mouth[2];
#ifdef __cplusplus
}
#endif
