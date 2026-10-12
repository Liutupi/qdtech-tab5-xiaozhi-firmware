#pragma once
#include "nabo_frame_composer.h"

namespace nabo_sd {
struct WideLayout {
    static constexpr int kSrcW = 320, kSrcH = 349;
    static constexpr int kOutW = 512, kOutH = 558;
    static constexpr int kHeadBottom = 423;
    static constexpr int kBodyX0 = 12, kBodyX1 = 490, kBodyY0 = 354, kBodyY1 = 539;
    static constexpr size_t kSrcBytes = size_t(kSrcW) * kSrcH * 3;
    static constexpr size_t kOutBytes = size_t(kOutW) * kOutH * 2;
};
class WideComposer {
public:
    using L = WideLayout;
    WideComposer(const uint16_t* background, const uint8_t* overlay)
        : background_(background), overlay_(overlay) {
        for (int x = 0; x < L::kOutW; ++x) {
            sx_[0][x] = x * L::kSrcW / L::kOutW;
            const int xx = (x * 100 - 256 * 6) / 94 - 39;
            sx_[1][x] = xx < 0 || xx >= 434 ? -1 : std::min(319, xx * 256 / 347);
        }
        for (int y = 0; y < L::kOutH; ++y) {
            sy_[0][y] = y * L::kSrcH / L::kOutH;
            const int yy = (y * 100 - 534 * 6) / 94;
            sy_[1][y] = yy < 0 || yy >= 558 ? -1 : std::min(411, yy * 256 / 347);
        }
    }
    void SetBackground(const uint16_t* background) { background_.store(background); }
    void SetLegacy(bool legacy) { legacy_ = legacy; }
    uint8_t AlphaAt(const uint8_t* src, int x, int y) const {
        int sx, sy;
        if (!Map(x, y, sx, sy))
            return 0;
        const int w = 320, h = legacy_ ? 412 : L::kSrcH;
        return src[size_t(w) * h * 2 + size_t(sy) * w + sx];
    }
#if defined(__GNUC__) && !defined(__clang__)
    __attribute__((optimize("O2")))
#endif
    void Compose(const uint8_t* src, uint8_t* out) const {
        const auto* color = reinterpret_cast<const uint16_t*>(src);
        const int h = legacy_ ? 412 : L::kSrcH;
        const uint8_t* alpha = src + size_t(320) * h * 2;
        const uint16_t* bg = background_.load();
        auto* dst = reinterpret_cast<uint16_t*>(out);
        uint16_t row[L::kOutW], colors[320];
        int16_t scaled_x[L::kOutW];
        uint8_t alphas[320], mask[L::kOutW];
        // Keep the hot loop in internal task-stack RAM. Calculating the legacy
        // transform and touching the PSRAM overlay for every output pixel cost
        // most of the worker's frame time on P4.
        std::memcpy(scaled_x, sx_[legacy_], sizeof(scaled_x));
        int loaded = -1;
        for (int y = 0; y < L::kOutH; ++y) {
            const uint16_t* background = bg + size_t(y) * L::kOutW;
            uint16_t* target = dst + size_t(y) * L::kOutW;
            const int sy = sy_[legacy_][y];
            if (sy < 0 || y >= L::kBodyY1) {
                std::memcpy(target, background, sizeof(row));
                continue;
            }
            std::memcpy(row, background, sizeof(row));
            if (loaded != sy) {
                loaded = sy;
                std::memcpy(colors, color + size_t(sy) * 320, sizeof(colors));
                std::memcpy(alphas, alpha + size_t(sy) * 320, sizeof(alphas));
            }
            if (overlay_)
                std::memcpy(mask, overlay_ + size_t(y) * L::kOutW, sizeof(mask));
            const int x0 = y >= L::kHeadBottom ? L::kBodyX0 : 0;
            const int x1 = y >= L::kHeadBottom ? L::kBodyX1 : L::kOutW;
            for (int x = x0; x < x1; ++x) {
                const int sx = scaled_x[x];
                if (sx < 0 || (overlay_ && mask[x]))
                    continue;
                const uint8_t a = alphas[sx];
                if (a > 3)
                    row[x] = a >= 252 ? colors[sx] : FrameComposer::Blend(colors[sx], row[x], a);
            }
            std::memcpy(target, row, sizeof(row));
        }
    }

private:
    bool Map(int x, int y, int& sx, int& sy) const {
        if (x < 0 || y < 0 || x >= L::kOutW || y >= L::kOutH)
            return false;
        sx = sx_[legacy_][x];
        sy = sy_[legacy_][y];
        return sx >= 0 && sy >= 0;
    }
    std::atomic<const uint16_t*> background_;
    const uint8_t* overlay_;
    // Both layouts preserve the exact original integer sampling, including
    // invalid legacy margins. The worker alone selects the active layout.
    int16_t sx_[2][L::kOutW], sy_[2][L::kOutH];
    bool legacy_ = true;
};
}  // namespace nabo_sd
