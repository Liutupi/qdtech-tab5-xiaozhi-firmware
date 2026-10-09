#pragma once
#include <stddef.h>
#include <stdint.h>

// Mix the core's mono FM and PSG streams, resample one emulated frame to
// 24 kHz, and duplicate to the two physical DAC slots. Fixed-size caller buffers.
static inline int32_t md_sample_at(const int16_t* src, size_t count, size_t i, size_t out_count) {
    if (!src || !count || !out_count)
        return 0;
    const size_t pos = i * count;
    const size_t a = pos / out_count;
    const size_t b = a + 1 < count ? a + 1 : a;
    const int32_t frac = (int32_t)(pos % out_count);
    return src[a] + (int32_t)((src[b] - src[a]) * frac / (int32_t)out_count);
}
static inline void md_mix_audio(const int16_t* fm, size_t fm_count, const int16_t* psg,
                                size_t psg_count, int16_t* out, size_t out_count) {
    for (size_t i = 0; i < out_count; ++i) {
        int32_t sample = md_sample_at(fm, fm_count, i, out_count) / 2 +
                         md_sample_at(psg, psg_count, i, out_count) / 2;
        if (sample > 32767)
            sample = 32767;
        if (sample < -32768)
            sample = -32768;
        out[2 * i] = out[2 * i + 1] = (int16_t)sample;
    }
}
