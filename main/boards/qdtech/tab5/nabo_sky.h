#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Host-tested sky behind Nabo: time of day, weather and the real moon phase.
// Pure C++, no ESP/LVGL.
//   - SelectSky(): which sky to show.
//   - RenderSkyBase(): the static layer (gradient, sun or phased moon, skyline) in RGB565,
//     drawn once per change. It dissolves into the page colour over a wide soft edge,
//     so the panel has no visible frame.
//   - SkyFx: everything that moves (drifting clouds, stars, lit windows, rain, snow,
//     birds), drawn into each composed video frame only where Nabo is fully transparent
//     and above the window, and faded by the same soft edge.
// Per-frame paths run on the SD reader next to the compositor: build them at -O2 even
// in the size-optimized firmware.
#if defined(__GNUC__) && !defined(__clang__)
#define NABO_SKY_HOT __attribute__((optimize("O2")))
#else
#define NABO_SKY_HOT
#endif

namespace nabo_sky {

enum class Period : uint8_t { Day, Dusk, Night };
enum class Weather : uint8_t { Clear, Cloudy, Rain, Snow };

struct Sky {
    Period period = Period::Day;
    Weather weather = Weather::Clear;
    uint8_t moon_day = 15;  // lunar age in days 0..29 (0 new, ~15 full)
    bool operator==(const Sky& o) const {
        return period == o.period && weather == o.weather && moon_day == o.moon_day;
    }
    bool operator!=(const Sky& o) const { return !(*this == o); }
};

// WMO weather interpretation codes (Open-Meteo).
inline Weather WeatherFromWmo(int code) {
    if (code < 0 || code <= 1)
        return Weather::Clear;
    if (code == 2 || code == 3 || code == 45 || code == 48)
        return Weather::Cloudy;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86)
        return Weather::Snow;
    return Weather::Rain;  // drizzle, rain, showers, thunderstorm
}

// minutes: local minutes since midnight. Dawn and dusk share the warm "Dusk" palette.
inline Period PeriodAt(int minutes, int sunrise = 6 * 60, int sunset = 18 * 60 + 15) {
    if (minutes < sunrise - 30 || minutes > sunset + 40)
        return Period::Night;
    if (std::abs(minutes - sunset) <= 40 || std::abs(minutes - sunrise) <= 30)
        return Period::Dusk;
    return Period::Day;
}

// Lunar age in days for a Unix time (mean synodic month from the 2000-01-06 new moon).
inline double MoonAge(int64_t unix_seconds) {
    constexpr double kSynodic = 29.530588853, kNewMoon2000 = 947182440.0;
    double age = std::fmod((double(unix_seconds) - kNewMoon2000) / 86400.0, kSynodic);
    return age < 0 ? age + kSynodic : age;
}

inline Sky SelectSky(int minutes, int sunrise, int sunset, int wmo_code, int64_t unix_seconds) {
    const int day = int(std::lround(MoonAge(unix_seconds))) % 30;
    return {PeriodAt(minutes, sunrise, sunset), WeatherFromWmo(wmo_code), uint8_t(day)};
}

// ---------------------------------------------------------------- colour helpers
struct Rgb {
    float r, g, b;
};
inline Rgb Hex(uint32_t h) { return {float((h >> 16) & 255), float((h >> 8) & 255), float(h & 255)}; }
inline Rgb Mix(Rgb a, Rgb b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
inline uint16_t To565(Rgb c) {
    auto q = [](float v, int bits) {
        const int m = (1 << bits) - 1;
        return std::clamp(int(v * m / 255.0f + 0.5f), 0, m);
    };
    return uint16_t((q(c.r, 5) << 11) | (q(c.g, 6) << 5) | q(c.b, 5));
}
// Ordered-dithered RGB565: long dark ramps (gradient, glow, edge dissolve) show no rings.
inline uint16_t To565Dither(Rgb c, int x, int y) {
    static constexpr uint8_t kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    const float d = kBayer[y & 3][x & 3] / 16.0f - .47f;
    return To565({c.r + d * 8.2f, c.g + d * 4.0f, c.b + d * 8.2f});
}
inline Rgb From565(uint16_t p) {
    return {float(((p >> 11) & 31) * 255 / 31), float(((p >> 5) & 63) * 255 / 63),
            float((p & 31) * 255 / 31)};
}
inline uint16_t Blend565(uint16_t fg, uint16_t bg, unsigned alpha) {
    if (alpha >= 252)
        return fg;
    if (alpha <= 3)
        return bg;
    const uint32_t a5 = (alpha + 4) >> 3;
    const uint32_t f = (fg | (uint32_t(fg) << 16)) & 0x07E0F81Fu;
    const uint32_t b = (bg | (uint32_t(bg) << 16)) & 0x07E0F81Fu;
    const uint32_t r = ((f * a5 + b * (32 - a5)) >> 5) & 0x07E0F81Fu;
    return uint16_t(r | (r >> 16));
}

// Small deterministic generator: the same sky always gets the same stars and skyline.
struct Lcg {
    uint32_t s;
    uint32_t Next() { return s = s * 1664525u + 1013904223u; }
    float Unit() { return float(Next() >> 8) / float(1u << 24); }
    int Range(int lo, int hi) { return lo + int(Unit() * float(hi - lo + 1)); }
};

constexpr uint32_t kPage = 0x0d1b2b;  // home page background around the panel
constexpr int kPanelW = 512, kPanelH = 558;
constexpr int kHorizon = 345;  // skyline base, just under the window top (340)
constexpr int kFxBottom = 338; // moving details stay above the window frame
constexpr int kFadeSide = 90, kFadeTop = 90, kFadeBottom = 50;

// 1 inside the panel, easing to 0 at its outer edges: the sky dissolves into the page.
inline float EdgeFactor(int x, int y) {
    auto ease = [](float v) { v = std::clamp(v, 0.0f, 1.0f); return v * v * (3 - 2 * v); };
    const float side = ease(float(std::min(x, kPanelW - 1 - x)) / kFadeSide);
    const float top = ease(float(y) / kFadeTop);
    const float bottom = ease(float(kPanelH - 1 - y) / kFadeBottom);
    return side * top * bottom;
}

// Composed video frames cover panel x 39..472; moving details fade out before that edge
// so nothing is cut off where the static sky image takes over.
constexpr int kVideoLeft = 39, kVideoRight = 472;
inline float VideoEdgeFactor(int x) {
    float v = std::clamp(float(std::min(x - kVideoLeft, kVideoRight - x)) / 60.0f, 0.0f, 1.0f);
    return v * v * (3 - 2 * v);
}

// The sun and moon sit far in the upper-right corner, well clear of Nabo's cap, and are
// drawn after the dissolve with only a narrow fade of their own: a bright distant body
// with a soft halo, while the sky around it melts into the page.
constexpr int kOrbX = 462, kOrbY = 50;
inline float OrbFade(int x, int y) {
    auto ease = [](float v) { v = std::clamp(v, 0.0f, 1.0f); return v * v * (3 - 2 * v); };
    return ease(float(std::min(x, kPanelW - 1 - x)) / 24.0f) * ease(float(y) / 24.0f);
}
inline void DrawHalo(uint16_t* out, int w, int h, int cx, int cy, int radius, uint32_t color, float strength) {
    const Rgb tint = Hex(color);
    for (int y = std::max(0, cy - radius); y < std::min(h, cy + radius); ++y)
        for (int x = std::max(0, cx - radius); x < std::min(w, cx + radius); ++x) {
            const float d = std::hypot(float(x - cx), float(y - cy)) / radius;
            if (d >= 1)
                continue;
            const float a = (1 - d) * (1 - d) * strength * OrbFade(x, y);
            if (a < .004f)
                continue;
            out[y * w + x] = To565Dither(Mix(From565(out[y * w + x]), tint, a), x, y);
        }
}

struct Palette {
    uint32_t top, bottom, skyline, cloud;
    uint8_t cloud_alpha;
    uint32_t glow;
    float glow_strength;
    int glow_x, glow_y, glow_r;
    bool sun, moon;
};

inline Palette PaletteFor(Sky sky) {
    const bool night = sky.period == Period::Night, dusk = sky.period == Period::Dusk;
    switch (sky.weather) {
        case Weather::Rain:
            return night ? Palette{0x141f30, 0x0e1a29, 0x0b1424, 0x2a3646, 210, 0, 0, 0, 0, 0, false, false}
                         : Palette{0x2a3b52, 0x182a3e, 0x1a2a3d, 0x4b5d74, 210, 0, 0, 0, 0, 0, false, false};
        case Weather::Snow:
            return night ? Palette{0x2a3a52, 0x15243a, 0x14223a, 0x55657e, 180, 0, 0, 0, 0, 0, false, false}
                         : Palette{0x56708d, 0x22384f, 0x2c4560, 0x8094ab, 180, 0, 0, 0, 0, 0, false, false};
        case Weather::Cloudy:
            if (night)
                return {0x101c33, 0x15263d, 0x0b1730, 0x283a55, 200, 0x9fc3ff, .12f, 462, 50, 120, false, true};
            if (dusk)
                return {0x3a3468, 0x8c6670, 0x2e2a4f, 0xb48b98, 190, 0xf0a07a, .25f, 380, 300, 170, false, false};
            return {0x4f6c88, 0x1d3147, 0x22384f, 0x8a9db2, 200, 0, 0, 0, 0, 0, false, false};
        case Weather::Clear:
        default:
            if (night)
                return {0x0a1430, 0x162d4b, 0x0b1730, 0x2b3f63, 140, 0x9fc3ff, .28f, 462, 50, 130, false, true};
            if (dusk)
                return {0x3a3468, 0xb7746a, 0x2e2a4f, 0xf3b7a6, 120, 0xf0a07a, .45f, 380, 300, 170, false, false};
            return {0x3f87bd, 0x16324d, 0x1d4466, 0xeaf3fb, 150, 0xffe7b0, .55f, 458, 52, 140, true, false};
    }
}

// Moon with the phase of lunar age `day` (0..29): lit limb on the right while waxing,
// on the left while waning; the dark part keeps a faint earthshine.
inline void DrawMoon(uint16_t* out, int w, int h, int cx, int cy, int r, int day) {
    const float phase = float(day) / 29.53f;           // 0 new, .5 full
    const float k = std::cos(phase * 2.0f * 3.14159265f);  // terminator position
    const uint16_t lit = To565(Hex(0xf4f1da)), dark = To565(Hex(0x2a3a5c));
    for (int y = cy - r - 1; y <= cy + r + 1; ++y)
        for (int x = cx - r - 1; x <= cx + r + 1; ++x) {
            if (x < 0 || y < 0 || x >= w || y >= h)
                continue;
            const float d = std::hypot(float(x - cx), float(y - cy));
            const float edge = std::clamp(r + .5f - d, 0.0f, 1.0f);
            if (edge <= 0)
                continue;
            const float u = float(x - cx) / r, v = float(y - cy) / r;
            const float half = std::sqrt(std::max(0.0f, 1 - v * v));
            // Soft 1.5 px terminator.
            const float s = phase < .5f ? (u - k * half) : (-k * half - u);
            const float lit_amount = std::clamp(s * r / 1.5f + .5f, 0.0f, 1.0f);
            const uint16_t c = Blend565(lit, dark, unsigned(lit_amount * 255));
            out[y * w + x] = Blend565(c, out[y * w + x], unsigned(edge * OrbFade(x, y) * (lit_amount > .02f ? 255 : 120)));
        }
}

// Static layer for the whole panel (kPanelW x kPanelH, RGB565). Clouds are not part of
// it: they move, so SkyFx draws them into the composed frames.
inline void RenderSkyBase(Sky sky, uint16_t* out, int w = kPanelW, int h = kPanelH) {
    const Palette p = PaletteFor(sky);
    const Rgb top = Hex(p.top), bottom = Hex(p.bottom), glow = Hex(p.glow);
    for (int y = 0; y < h; ++y) {
        const Rgb row = Mix(top, bottom, float(y) / float(h - 1));
        for (int x = 0; x < w; ++x) {
            Rgb c = row;
            if (p.glow_strength > 0) {
                const float dx = float(x - p.glow_x) / p.glow_r, dy = float(y - p.glow_y) / p.glow_r;
                const float a = std::min(1.0f, std::exp(-(dx * dx + dy * dy) * 2.2f) * p.glow_strength);
                c = Mix(c, glow, a);
            }
            out[y * w + x] = To565Dither(c, x, y);
        }
    }
    // Distant skyline.
    Lcg rng{11};
    const Rgb base_building = Hex(p.skyline);
    for (int x = -4; x < w;) {
        const int bw = rng.Range(26, 54), bh = rng.Range(40, 120), top_y = kHorizon - bh;
        Lcg shade{uint32_t(x + 1000) * 2654435761u};
        const float k = .9f + .2f * shade.Unit();
        const uint16_t building = To565({base_building.r * k, base_building.g * k, base_building.b * k});
        for (int y = std::max(0, top_y); y < h; ++y)
            for (int xx = std::max(0, x); xx <= std::min(w - 1, x + bw); ++xx)
                out[y * w + xx] = building;
        x += bw + rng.Range(0, 3);
    }
    // Dissolve into the page colour: no visible panel edge. Mixed in full precision and
    // ordered-dithered to RGB565 so the long dark ramp shows no rings.
    const Rgb page = Hex(kPage);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float f = EdgeFactor(x, y);
            if (f >= 1.0f)
                continue;
            out[y * w + x] = f <= 0 ? To565(page) : To565Dither(Mix(page, From565(out[y * w + x]), f), x, y);
        }
    // Sun or moon last, over the dissolve: far away in the corner, clear of the cap.
    if (p.sun) {
        DrawHalo(out, w, h, kOrbX, kOrbY, 70, 0xffe7b0, .45f);
        const int r = 18;
        const uint16_t disk = To565(Hex(0xfff3d1));
        for (int y = kOrbY - r - 1; y <= kOrbY + r + 1; ++y)
            for (int x = kOrbX - r - 1; x <= kOrbX + r + 1; ++x) {
                const float a = std::clamp(r + .5f - std::hypot(float(x - kOrbX), float(y - kOrbY)), 0.0f, 1.0f);
                if (a > 0 && x >= 0 && y >= 0 && x < w && y < h)
                    out[y * w + x] = Blend565(disk, out[y * w + x], unsigned(a * OrbFade(x, y) * 255));
            }
    }
    if (p.moon && sky.moon_day >= 1 && sky.moon_day <= 28) {  // no moon at new moon
        const float full = 1 - std::abs(sky.moon_day - 14.77f) / 14.77f;  // brighter near full
        DrawHalo(out, w, h, kOrbX, kOrbY, 56, 0xbcd4ff, .12f + .22f * full);
        DrawMoon(out, w, h, kOrbX, kOrbY, 16, sky.moon_day);
    }
}

// ---------------------------------------------------------------- moving details
struct Point {
    int16_t x, y;
    uint8_t phase, size;
};

class SkyFx {
public:
    static constexpr int kStars = 46, kLights = 40, kDrops = 70, kFlakes = 55, kClouds = 3;
    static constexpr int kCloudW = 216, kCloudH = 120;

    // Empty until Reset(); draws nothing. Rebuild in place with Reset(): clouds are
    // ~78 KB, so never construct temporaries on a task stack.
    SkyFx() = default;
    void Reset(Sky sky) {
        sky_ = sky;
        for (int x = 0; x < kPanelW; ++x)
            col_fade_[x] = uint8_t(255 * EdgeFactor(x, kPanelH / 2) * VideoEdgeFactor(x));
        for (int y = 0; y < kFxBottom; ++y)
            row_fade_[y] = uint8_t(255 * EdgeFactor(kPanelW / 2, y));
        palette_ = PaletteFor(sky);
        Lcg rng{7};
        for (auto& s : stars_)
            s = {int16_t(rng.Range(8, 504)), int16_t(rng.Range(8, 300)), uint8_t(rng.Next() >> 24),
                 uint8_t(rng.Range(0, 6) == 0 ? 2 : 1)};
        Lcg sk{11}, pick{23};
        int n = 0;
        for (int x = -4; x < kPanelW && n < kLights;) {
            const int bw = sk.Range(26, 54), bh = sk.Range(40, 120), top_y = kHorizon - bh;
            for (int wy = top_y + 8; wy < kFxBottom - 4 && n < kLights; wy += 12)
                for (int wx = x + 5; wx < x + bw - 5 && n < kLights; wx += 9)
                    if (pick.Unit() < .14f)
                        lights_[n++] = {int16_t(wx), int16_t(wy), uint8_t(pick.Next() >> 24), 0};
            x += bw + sk.Range(0, 3);
        }
        light_count_ = n;
        for (auto& d : drops_)
            d = {int16_t(rng.Range(-60, 512)), int16_t(rng.Range(0, 340)), uint8_t(rng.Range(80, 120)), 0};
        for (auto& f : flakes_)
            f = {int16_t(rng.Range(0, 512)), int16_t(rng.Range(0, 340)), uint8_t(rng.Range(50, 100)),
                 uint8_t(rng.Range(1, 3))};
        BuildClouds();
    }
    Sky sky() const { return sky_; }

    // out: composed frame (out_w x out_h) whose left edge is at origin_x in panel
    // coordinates. alpha_at(x, y): Nabo's opacity at that output pixel (0 = sky shows).
    template <typename AlphaAt>
    NABO_SKY_HOT void Draw(uint32_t t_ms, uint16_t* out, int out_w, int out_h, int origin_x, AlphaAt alpha_at) const {
        DrawClouds(t_ms, out, out_w, out_h, origin_x, alpha_at);
        auto plot = [&](int px, int py, uint16_t color, unsigned alpha) {
            const int x = px - origin_x;
            if (x < 0 || x >= out_w || py < 0 || py >= std::min(out_h, kFxBottom) || alpha_at(x, py) > 8)
                return;
            out[py * out_w + x] = Blend565(color, out[py * out_w + x], Fade(alpha, px, py));
        };
        const float t = float(t_ms % 600000u) / 1000.0f;
        const bool night = sky_.period == Period::Night;
        if (sky_.weather == Weather::Clear && night) {
            const uint16_t star = To565(Hex(0xebf2ff));
            for (const auto& s : stars_) {
                const float tw = .35f + .65f * (.5f + .5f * std::sin(t * 1.3f + s.phase * .0245f));
                plot(s.x, s.y, star, unsigned(255 * tw));
                if (s.size > 1) {
                    plot(s.x + 1, s.y, star, unsigned(160 * tw));
                    plot(s.x, s.y + 1, star, unsigned(160 * tw));
                }
            }
        }
        if (night && sky_.weather != Weather::Snow) {
            const uint16_t lamp = To565(Hex(0xffd68c));
            for (int i = 0; i < light_count_; ++i) {
                const auto& l = lights_[i];
                const float f = .55f + .45f * std::sin(t * .6f + l.phase * .0245f);
                for (int dy = 0; dy < 4; ++dy)
                    for (int dx = 0; dx < 3; ++dx)
                        plot(l.x + dx, l.y + dy, lamp, unsigned(190 * f));
            }
        }
        if (sky_.weather == Weather::Rain) {
            const uint16_t rain = To565(Hex(night ? 0x8fa6c2 : 0xb4cde6));
            for (const auto& d : drops_) {
                const float speed = d.phase / 100.0f;
                const int y = int(d.y + t * 330 * speed) % 360 - 20;
                const int x = d.x + int((y - d.y) * .18f) % 520;
                for (int k = 0; k < 18; ++k) {
                    plot(x - k / 5, y + k, rain, 110);
                    plot(x - k / 5 + 1, y + k, rain, 60);
                }
            }
        }
        if (sky_.weather == Weather::Snow) {
            const uint16_t snow = To565(Hex(0xf5f8ff));
            for (int i = 0; i < kFlakes; ++i) {
                const auto& f = flakes_[i];
                const int y = int(f.y + t * 34 * (f.phase / 100.0f)) % 350;
                const int x = f.x + int(std::sin(y / 60.0f + i) * 8);
                for (int dy = -f.size; dy <= f.size; ++dy)
                    for (int dx = -f.size; dx <= f.size; ++dx)
                        if (dx * dx + dy * dy <= f.size * f.size)
                            plot(x + dx, y + dy, snow, 220);
            }
        }
        if (sky_.weather == Weather::Clear && !night) {
            // Two small birds glide across the sky, about one pass a minute.
            const uint16_t bird = To565(Hex(sky_.period == Period::Dusk ? 0x2c2440 : 0x1f3550));
            for (int b = 0; b < 2; ++b) {
                const float cycle = std::fmod(t + b * 3.0f, 60.0f);
                if (cycle > 14.0f)
                    continue;
                const int x = int(-20 + cycle * 40) + b * 18, y = 150 + b * 10 + int(std::sin(cycle * 2) * 4);
                const int wing = (int(cycle * 6) & 1) ? 2 : 4;
                for (int k = 0; k <= 5; ++k) {
                    plot(x - k, y - (k * wing) / 5, bird, 230);
                    plot(x + k, y - (k * wing) / 5, bird, 230);
                }
            }
        }
    }

private:
    // alpha scaled by the soft panel edge and the video edge (px in panel coords).
    unsigned Fade(unsigned alpha, int px, int py) const {
        if (px < 0 || px >= kPanelW || py < 0 || py >= kFxBottom)
            return 0;
        return alpha * col_fade_[px] * row_fade_[py] / (255 * 255);
    }

    struct Cloud {
        int w = 0, h = 0;          // mask size actually used
        float x0 = 0, y = 0;       // start x (panel) and top
        float speed = 0;           // px per second, drifting left
        uint8_t alpha = 0;
    };

    // Soft cloud masks: five overlapping feathered ellipses, rendered once per sky.
    void BuildClouds() {
        static constexpr float kStart[kClouds][3] = {{120, 120, 46}, {330, 200, 34}, {40, 250, 30}};
        const bool heavy = sky_.weather != Weather::Clear;
        cloud_count_ = sky_.weather == Weather::Clear && sky_.period == Period::Night ? 1 : kClouds;
        cloud_color_ = To565(Hex(palette_.cloud));
        static constexpr float kParts[5][3] = {
            {-1.1f, .25f, .55f}, {-.45f, -.15f, .75f}, {.35f, -.05f, .68f}, {1.0f, .3f, .5f}, {0, .35f, .7f}};
        for (int i = 0; i < kClouds; ++i) {
            const float s = std::min(58.0f, kStart[i][2] * (heavy ? 1.25f : 1.0f));
            Cloud& c = clouds_[i];
            c.w = std::min(kCloudW, int(3.6f * s) + 2);
            c.h = std::min(kCloudH, int(2.0f * s) + 2);
            c.x0 = kStart[i][0] + (heavy ? i * 40 : 0) - c.w / 2.0f;
            c.y = (heavy ? 55.0f + i * 28 : kStart[i][1]) - c.h / 2.0f;
            c.speed = (heavy ? 3.0f : 5.0f) + i * 1.5f;
            c.alpha = uint8_t(palette_.cloud_alpha - i * 18);
            const float cx = c.w / 2.0f, cy = c.h / 2.0f;
            for (int y = 0; y < c.h; ++y)
                for (int x = 0; x < c.w; ++x) {
                    float cover = 0;
                    for (const auto& p : kParts) {
                        const float rx = p[2] * s, ry = p[2] * s * .72f;
                        const float dx = (x - (cx + p[0] * s)) / rx, dy = (y - (cy + p[1] * s)) / ry;
                        cover = std::max(cover, std::clamp((1.0f - std::sqrt(dx * dx + dy * dy)) / .35f, 0.0f, 1.0f));
                    }
                    masks_[i][y * kCloudW + x] = uint8_t(cover * 255);
                }
        }
    }

    template <typename AlphaAt>
    NABO_SKY_HOT void DrawClouds(uint32_t t_ms, uint16_t* out, int out_w, int out_h, int origin_x, AlphaAt alpha_at) const {
        const float t = float(t_ms % 3600000u) / 1000.0f;
        for (int i = 0; i < cloud_count_; ++i) {
            const Cloud& c = clouds_[i];
            // Drift left across the panel and wrap; the soft edge hides entry and exit.
            const float span = float(kPanelW + c.w);
            const float pos = std::fmod(c.x0 + c.w - t * c.speed, span);
            const int left = int((pos < 0 ? pos + span : pos) - c.w);
            const int top = int(c.y);
            for (int y = 0; y < c.h; ++y) {
                const int py = top + y;
                if (py < 0 || py >= std::min(out_h, kFxBottom))
                    continue;
                for (int x = 0; x < c.w; ++x) {
                    const uint8_t m = masks_[i][y * kCloudW + x];
                    if (m < 4)
                        continue;
                    const int px = left + x, ox = px - origin_x;
                    if (ox < 0 || ox >= out_w || alpha_at(ox, py) > 8)
                        continue;
                    out[py * out_w + ox] = Blend565(cloud_color_, out[py * out_w + ox],
                                                    Fade(m * c.alpha / 255, px, py));
                }
            }
        }
    }

    Sky sky_{};
    Palette palette_{};
    std::array<uint8_t, kPanelW> col_fade_{};
    std::array<uint8_t, kFxBottom> row_fade_{};
    std::array<Point, kStars> stars_{};
    std::array<Point, kLights> lights_{};
    int light_count_ = 0;
    std::array<Point, kDrops> drops_{};
    std::array<Point, kFlakes> flakes_{};
    std::array<Cloud, kClouds> clouds_{};
    int cloud_count_ = 0;
    uint16_t cloud_color_ = 0;
    std::array<std::array<uint8_t, kCloudW * kCloudH>, kClouds> masks_{};
};

}  // namespace nabo_sky
