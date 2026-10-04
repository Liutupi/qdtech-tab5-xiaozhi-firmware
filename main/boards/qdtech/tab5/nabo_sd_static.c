#include "nabo_sd_static.h"
extern const uint8_t nabo_sd_neutral_start[];
const lv_image_dsc_t nabo_sd_neutral = {.header.magic = LV_IMAGE_HEADER_MAGIC,
                                        .header.cf = LV_COLOR_FORMAT_RGB565A8,
                                        .header.w = 320,
                                        .header.h = 412,
                                        .header.stride = 640,
                                        .data_size = 395520,
                                        .data = nabo_sd_neutral_start};
extern const uint8_t nabo_sd_half_blink_start[];
const lv_image_dsc_t nabo_sd_half_blink = {.header.magic = LV_IMAGE_HEADER_MAGIC,
                                           .header.cf = LV_COLOR_FORMAT_RGB565A8,
                                           .header.w = 201,
                                           .header.h = 94,
                                           .header.stride = 402,
                                           .data_size = 56682,
                                           .data = nabo_sd_half_blink_start};
extern const uint8_t nabo_sd_closed_blink_start[];
const lv_image_dsc_t nabo_sd_closed_blink = {.header.magic = LV_IMAGE_HEADER_MAGIC,
                                             .header.cf = LV_COLOR_FORMAT_RGB565A8,
                                             .header.w = 201,
                                             .header.h = 94,
                                             .header.stride = 402,
                                             .data_size = 56682,
                                             .data = nabo_sd_closed_blink_start};
extern const uint8_t nabo_sd_mouth_half_start[];
const lv_image_dsc_t nabo_sd_mouth_half = {.header.magic = LV_IMAGE_HEADER_MAGIC,
                                           .header.cf = LV_COLOR_FORMAT_RGB565A8,
                                           .header.w = 62,
                                           .header.h = 41,
                                           .header.stride = 124,
                                           .data_size = 7626,
                                           .data = nabo_sd_mouth_half_start};
extern const uint8_t nabo_sd_mouth_open_start[];
const lv_image_dsc_t nabo_sd_mouth_open = {.header.magic = LV_IMAGE_HEADER_MAGIC,
                                           .header.cf = LV_COLOR_FORMAT_RGB565A8,
                                           .header.w = 61,
                                           .header.h = 42,
                                           .header.stride = 122,
                                           .data_size = 7686,
                                           .data = nabo_sd_mouth_open_start};
const nabo_sd_patch_t nabo_sd_eyes[2] = {{&nabo_sd_half_blink, 126, 251},
                                         {&nabo_sd_closed_blink, 126, 251}};
const nabo_sd_patch_t nabo_sd_mouth[2] = {{&nabo_sd_mouth_half, 204, 328},
                                          {&nabo_sd_mouth_open, 205, 327}};
