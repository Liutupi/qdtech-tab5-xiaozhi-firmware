#pragma once

#include "nabo_framepack.h"

// The push-page portrait is composed on the SD worker, never scaled by LVGL.
namespace nabo_avatar {
constexpr int kWidth = 160, kHeight = 174;
constexpr uint16_t kBackground = 0x10e4;  // RGB565 of the push card's #171c25.

inline bool Compose(const nabo_sd::Entry& entry, const uint8_t* raw, int width, int height,
                    uint16_t* out) {
    if (!raw || !out || width <= 0 || width > 320 || height <= 0 || height > 412 || !entry.width ||
        !entry.height || int(entry.x) + entry.width > width || int(entry.y) + entry.height > height)
        return false;
    const int w = std::min(kWidth, width * kHeight / height);
    const int h = std::min(kHeight, height * kWidth / width);
    if (!w || !h)
        return false;
    const int left = (kWidth - w) / 2, top = (kHeight - h) / 2;
    std::fill_n(out, kWidth * kHeight, kBackground);
    const auto* colors = reinterpret_cast<const uint16_t*>(raw);
    const uint8_t* alpha = raw + size_t(entry.width) * entry.height * 2;
    // Area sampling keeps glasses, hair and transparent edges steady at half size.
    for (int y = 0; y < h; ++y) {
        const int y0 = y * height / h, y1 = std::max(y0 + 1, (y + 1) * height / h);
        for (int x = 0; x < w; ++x) {
            const int x0 = x * width / w, x1 = std::max(x0 + 1, (x + 1) * width / w);
            unsigned a = 0, r = 0, g = 0, b = 0;
            const unsigned samples = unsigned((x1 - x0) * (y1 - y0));
            for (int sy = std::max(y0, int(entry.y));
                 sy < std::min(y1, int(entry.y) + entry.height); ++sy) {
                for (int sx = std::max(x0, int(entry.x));
                     sx < std::min(x1, int(entry.x) + entry.width); ++sx) {
                    const size_t i = size_t(sy - entry.y) * entry.width + sx - entry.x;
                    const uint16_t c = colors[i];
                    const unsigned k = alpha[i];
                    a += k;
                    r += (c >> 11) * k;
                    g += ((c >> 5) & 63) * k;
                    b += (c & 31) * k;
                }
            }
            const unsigned denominator = samples * 255, background = denominator - a;
            r = (r + (kBackground >> 11) * background) / denominator;
            g = (g + ((kBackground >> 5) & 63) * background) / denominator;
            b = (b + (kBackground & 31) * background) / denominator;
            out[size_t(top + y) * kWidth + left + x] = uint16_t(r << 11 | g << 5 | b);
        }
    }
    return true;
}
}  // namespace nabo_avatar
