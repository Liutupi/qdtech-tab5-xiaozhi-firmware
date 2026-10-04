#pragma once

#include <cstddef>
#include <cstdint>

// Cache-friendly RGB565 rotation for direct SD video output. Produces exactly
// the result of LVGL's lv_draw_sw_rotate(..., LV_DISPLAY_ROTATION_270, RGB565):
//   dst[x * dst_stride + (h - 1 - y)] = src[y * src_stride + x]
// but walks 32x32 tiles so PSRAM reads and writes stay within a few cache lines
// instead of striding a whole source column per output row. Strides in pixels.
namespace nabo_sd {
#if defined(__GNUC__) && !defined(__clang__)
#define NABO_HOT __attribute__((optimize("O2")))
#else
#define NABO_HOT
#endif

NABO_HOT inline void Rotate270Rgb565(const uint16_t* src, uint16_t* dst, int w, int h,
                                     int src_stride, int dst_stride) {
    constexpr int kTile = 32;
    for (int y0 = 0; y0 < h; y0 += kTile) {
        const int y1 = y0 + kTile < h ? y0 + kTile : h;
        for (int x0 = 0; x0 < w; x0 += kTile) {
            const int x1 = x0 + kTile < w ? x0 + kTile : w;
            for (int x = x0; x < x1; ++x) {
                uint16_t* out = dst + size_t(x) * dst_stride + (h - 1);
                const uint16_t* in = src + size_t(y0) * src_stride + x;
                for (int y = y0; y < y1; ++y, in += src_stride)
                    out[-y] = *in;
            }
        }
    }
}
}  // namespace nabo_sd
