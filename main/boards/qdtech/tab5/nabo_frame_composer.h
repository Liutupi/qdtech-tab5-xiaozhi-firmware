#pragma once

#include <cstddef>
#include <atomic>
#include <cstdint>
#include <cstring>

// Host-tested worker-side compositor. Reproduces the original LVGL layering of
// the SD scene (scaled RGB565A8 frame over the static window art) once per frame
// on the reader task, so LVGL only blits an opaque RGB565 image:
//   - rows above kHeadBottom: the head layer draws over everything;
//   - lower rows: the body is visible only inside the window interior;
//   - decorations created after the head (posts, sill) are drawn on top: an
//     optional overlay mask marks their pixels, which keep the art unchanged,
//     so the output is the final on-screen region and can bypass LVGL.
// No allocation; all buffers are caller-owned.
namespace nabo_sd {
struct ComposeLayout {
    static constexpr int kSrcW = 320, kSrcH = 412;
    static constexpr int kOutW = 434, kOutH = 558;  // 320x412 at LVGL scale 347/256
    static constexpr int kScale = 347;
    static constexpr int kHeadBottom = 423;  // head clip height (local rows)
    // Window interior (body clip) in output-local coordinates: x 108-39, y 354.
    static constexpr int kBodyX0 = 69, kBodyX1 = 69 + 324, kBodyY0 = 354, kBodyY1 = 354 + 185;
    static constexpr size_t kSrcBytes = size_t(kSrcW) * kSrcH * 3;
    static constexpr size_t kOutBytes = size_t(kOutW) * kOutH * 2;
};

class FrameComposer {
public:
    using L = ComposeLayout;
    // background: opaque RGB565 kOutW x kOutH snapshot of the static scene art.
    // overlay: optional kOutW x kOutH bytes, non-zero where front decorations
    // cover the frame (the background already contains them there).
    explicit FrameComposer(const uint16_t* background, const uint8_t* overlay = nullptr)
        : background_(background), overlay_(overlay) {
        // background_ may be swapped later (SetBackground) from another task.
        // Nearest-neighbour, matching LVGL's unfiltered transform with pivot 0,0.
        for (int x = 0; x < L::kOutW; ++x) {
            const int s = x * 256 / L::kScale;
            sx_[x] = uint16_t(s < L::kSrcW ? s : L::kSrcW - 1);
        }
        for (int y = 0; y < L::kOutH; ++y) {
            const int s = y * 256 / L::kScale;
            sy_[y] = uint16_t(s < L::kSrcH ? s : L::kSrcH - 1);
        }
    }
    static inline uint16_t Blend(uint16_t fg, uint16_t bg, uint8_t alpha) {
        if (alpha >= 252)
            return fg;
        if (alpha <= 3)
            return bg;
        // Spread RGB565 to 0x07E0F81F so the three channels blend in one multiply.
        const uint32_t a5 = (uint32_t(alpha) + 4) >> 3;
        const uint32_t f = (fg | (uint32_t(fg) << 16)) & 0x07E0F81Fu;
        const uint32_t b = (bg | (uint32_t(bg) << 16)) & 0x07E0F81Fu;
        const uint32_t r = ((f * a5 + b * (32 - a5)) >> 5) & 0x07E0F81Fu;
        return uint16_t(r | (r >> 16));
    }
    // Swap the static art (e.g. a new sky). The caller writes the new buffer before
    // publishing it and keeps the previous one alive until at least one frame later.
    void SetBackground(const uint16_t* background) {
        background_.store(background, std::memory_order_release);
    }
    // Nabo's opacity at an output pixel inside the head rows (0 = background shows).
    uint8_t AlphaAt(const uint8_t* src, int x, int y) const {
        if (x < 0 || y < 0 || x >= L::kOutW || y >= L::kHeadBottom)
            return 255;
        const uint8_t* alpha = src + size_t(L::kSrcW) * L::kSrcH * 2;
        return alpha[size_t(sy_[y]) * L::kSrcW + sx_[x]];
    }
    // src: RGB565 plane (kSrcW*kSrcH*2) followed by A8 plane. out: kOutBytes.
    // Hot per-frame loop: built at -O2 even in the size-optimized firmware.
#if defined(__GNUC__) && !defined(__clang__)
    __attribute__((optimize("O2")))
#endif
    void Compose(const uint8_t* src, uint8_t* out) const {
        const auto* color = reinterpret_cast<const uint16_t*>(src);
        const uint8_t* alpha = src + size_t(L::kSrcW) * L::kSrcH * 2;
        auto* dst = reinterpret_cast<uint16_t*>(out);
        const uint16_t* const background = background_.load(std::memory_order_acquire);
        for (int y = 0; y < L::kOutH; ++y) {
            const uint16_t* bg = background + size_t(y) * L::kOutW;
            uint16_t* row = dst + size_t(y) * L::kOutW;
            int x0 = 0, x1 = L::kOutW;
            if (y >= L::kHeadBottom) {
                if (y < L::kBodyY0 || y >= L::kBodyY1) {
                    std::memcpy(row, bg, size_t(L::kOutW) * 2);
                    continue;
                }
                x0 = L::kBodyX0;
                x1 = L::kBodyX1;
                std::memcpy(row, bg, size_t(x0) * 2);
                std::memcpy(row + x1, bg + x1, size_t(L::kOutW - x1) * 2);
            }
            const size_t s = size_t(sy_[y]) * L::kSrcW;
            const uint16_t* crow = color + s;
            const uint8_t* arow = alpha + s;
            const uint8_t* orow = overlay_ ? overlay_ + size_t(y) * L::kOutW : nullptr;
            for (int x = x0; x < x1; ++x) {
                const unsigned i = sx_[x];
                row[x] = orow && orow[x] ? bg[x] : Blend(crow[i], bg[x], arow[i]);
            }
        }
    }

private:
    std::atomic<const uint16_t*> background_;
    const uint8_t* overlay_;
    uint16_t sx_[L::kOutW];
    uint16_t sy_[L::kOutH];
};
}  // namespace nabo_sd
