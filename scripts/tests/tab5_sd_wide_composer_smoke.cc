#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>
#include "nabo_wide_composer.h"

// Independent scalar geometry: pin the exact pixels while optimizing the P4
// compositor, including invalid legacy margins and front-window decorations.
static bool Sample(bool legacy, int x, int y, int& sx, int& sy) {
    if (x < 0 || y < 0 || x >= 512 || y >= 558)
        return false;
    if (legacy) {
        const int xx = (x * 100 - 256 * 6) / 94 - 39;
        const int yy = (y * 100 - 534 * 6) / 94;
        if (xx < 0 || xx >= 434 || yy < 0 || yy >= 558)
            return false;
        sx = std::min(319, xx * 256 / 347);
        sy = std::min(411, yy * 256 / 347);
    } else {
        sx = x * 320 / 512;
        sy = y * 349 / 558;
    }
    return true;
}

int main() {
    constexpr size_t pixels = 512 * 558;
    std::vector<uint16_t> bg(pixels), output(pixels), next_bg(pixels);
    std::vector<uint8_t> mask(pixels);
    for (size_t i = 0; i < pixels; ++i) {
        bg[i] = uint16_t(i * 919 + 71);
        next_bg[i] = uint16_t(~bg[i]);
        mask[i] = (i % 19 == 0) ? 255 : 0;
    }
    for (bool overlay : {false, true}) {
        nabo_sd::WideComposer composer(bg.data(), overlay ? mask.data() : nullptr);
        for (bool legacy : {true, false, true, false}) {
            const int height = legacy ? 412 : 349;
            const size_t plane = 320 * height;
            std::vector<uint8_t> src(plane * 3);
            auto* colors = reinterpret_cast<uint16_t*>(src.data());
            auto* alphas = src.data() + plane * 2;
            for (size_t i = 0; i < plane; ++i) {
                colors[i] = uint16_t(i * 617 + 33);
                alphas[i] = uint8_t(i * 13);
            }
            composer.SetLegacy(legacy);
            for (const auto* background : {bg.data(), next_bg.data()}) {
                composer.SetBackground(background);
                composer.Compose(src.data(), reinterpret_cast<uint8_t*>(output.data()));
                for (int y = 0; y < 558; ++y) {
                    for (int x = 0; x < 512; ++x) {
                        const size_t o = size_t(y) * 512 + x;
                        int sx, sy;
                        const bool valid = Sample(legacy, x, y, sx, sy);
                        const uint8_t a = valid ? alphas[size_t(sy) * 320 + sx] : 0;
                        assert(composer.AlphaAt(src.data(), x, y) == a);
                        uint16_t expected = background[o];
                        if (valid && !(overlay && mask[o]) &&
                            !(y >= 423 && (x < 12 || x >= 490 || y >= 539)))
                            expected = nabo_sd::FrameComposer::Blend(colors[size_t(sy) * 320 + sx],
                                                                     expected, a);
                        assert(output[o] == expected);
                    }
                }
                assert(composer.AlphaAt(src.data(), -1, 0) == 0);
                assert(composer.AlphaAt(src.data(), 0, -1) == 0);
                assert(composer.AlphaAt(src.data(), 512, 0) == 0);
                assert(composer.AlphaAt(src.data(), 0, 558) == 0);
            }
        }
    }
    std::puts("PASS exact wide/legacy pixels, alpha boundaries, overlay and background swaps");
}
