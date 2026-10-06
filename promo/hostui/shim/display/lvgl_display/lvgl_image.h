#pragma once
#include "lvgl.h"
class LvglImage { public: virtual const lv_image_dsc_t* image_dsc() const = 0; virtual ~LvglImage() = default; };
class LvglAllocatedImage : public LvglImage {
public:
    LvglAllocatedImage(void* data, size_t size, int w, int h, int stride, int cf) : data_(data) {
        dsc_.header.magic = LV_IMAGE_HEADER_MAGIC; dsc_.header.cf = cf; dsc_.header.w = w; dsc_.header.h = h; dsc_.header.stride = stride;
        dsc_.data = (const uint8_t*)data; dsc_.data_size = size; }
    const lv_image_dsc_t* image_dsc() const override { return &dsc_; }
private:
    void* data_; lv_image_dsc_t dsc_{};
};
