#include <cassert>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "nabo_frame_composer.h"
#include "nabo_scene_mailbox.h"
#include "nabo_sd_host_source.h"
using namespace nabo_sd;
using L = ComposeLayout;

// Independent per-pixel reference of the original LVGL layer rules.
static uint16_t Reference(const uint8_t* src, const uint16_t* bg, int x, int y,
                          const uint8_t* overlay = nullptr) {
    const uint16_t back = bg[y * L::kOutW + x];
    if (overlay && overlay[y * L::kOutW + x])
        return back;
    const bool head = y < L::kHeadBottom;
    const bool body = x >= L::kBodyX0 && x < L::kBodyX1 && y >= L::kBodyY0 && y < L::kBodyY1;
    if (!head && !body)
        return back;
    const int sx = std::min(x * 256 / L::kScale, L::kSrcW - 1);
    const int sy = std::min(y * 256 / L::kScale, L::kSrcH - 1);
    const auto* color = reinterpret_cast<const uint16_t*>(src);
    const uint8_t a = src[L::kSrcW * L::kSrcH * 2 + sy * L::kSrcW + sx];
    return FrameComposer::Blend(color[sy * L::kSrcW + sx], back, a);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    // Channel-exact blend extremes and a midpoint.
    assert(FrameComposer::Blend(0xffff, 0x0000, 255) == 0xffff);
    assert(FrameComposer::Blend(0xffff, 0x1234, 0) == 0x1234);
    const uint16_t mid = FrameComposer::Blend(0xf800, 0x001f, 128);
    assert((mid >> 11) >= 14 && (mid >> 11) <= 17 && (mid & 31) >= 14 && (mid & 31) <= 17);
    assert(L::kSrcW * L::kScale / 256 == L::kOutW - 1 || L::kSrcW * L::kScale / 256 == L::kOutW);

    std::vector<uint16_t> bg(L::kOutW * L::kOutH);
    for (size_t i = 0; i < bg.size(); ++i)
        bg[i] = uint16_t(i * 2654435761u >> 16);
    std::vector<uint8_t> src(L::kSrcBytes);
    for (size_t i = 0; i < src.size(); ++i)
        src[i] = uint8_t(i * 40503u >> 7);
    // Force fully transparent / opaque alpha runs to exercise fast paths.
    uint8_t* alpha = src.data() + L::kSrcW * L::kSrcH * 2;
    for (int i = 0; i < L::kSrcW * 40; ++i) {
        alpha[i] = 0;
        alpha[L::kSrcW * 200 + i] = 255;
    }
    auto* composer = new FrameComposer(bg.data());
    std::vector<uint8_t> out(L::kOutBytes);
    composer->Compose(src.data(), out.data());
    const auto* got = reinterpret_cast<const uint16_t*>(out.data());
    for (int y = 0; y < L::kOutH; ++y)
        for (int x = 0; x < L::kOutW; ++x)
            assert(got[y * L::kOutW + x] == Reference(src.data(), bg.data(), x, y));
    // Below the head, outside the window interior the static art is untouched.
    assert(got[500 * L::kOutW + 10] == bg[500 * L::kOutW + 10]);
    assert(got[L::kBodyY1 * L::kOutW + 200] == bg[L::kBodyY1 * L::kOutW + 200]);

    // Front decorations (overlay mask) always keep the art pixel.
    {
        std::vector<uint8_t> overlay(L::kOutW * L::kOutH);
        for (int y = 360; y < 547; ++y)
            for (int x = 55; x < 70; ++x)  // left post, crossing head and body rows
                overlay[y * L::kOutW + x] = 1;
        FrameComposer masked(bg.data(), overlay.data());
        masked.Compose(src.data(), out.data());
        for (int y = 0; y < L::kOutH; ++y)
            for (int x = 0; x < L::kOutW; ++x)
                assert(got[y * L::kOutW + x] ==
                       Reference(src.data(), bg.data(), x, y, overlay.data()));
        assert(got[400 * L::kOutW + 60] == bg[400 * L::kOutW + 60]);
    }

    // Mailbox composer path: read into aligned staging, publish composed slot.
    constexpr size_t align = 128, staging_bytes = L::kSrcBytes + align;
    FileSource source(argv[1]);
    Pack pack;
    assert(pack.Open(source, L::kSrcBytes, 10000) == Error::Ok);
    auto* staging = static_cast<uint8_t*>(std::aligned_alloc(align, staging_bytes + align));
    std::vector<uint8_t> a(L::kOutBytes), b(L::kOutBytes);
    SceneMailbox box(a.data(), b.data(), L::kOutBytes, align);
    box.SetComposer(staging, staging_bytes, [](void* c, const uint8_t* raw, uint8_t* o) {
        static_cast<FrameComposer*>(c)->Compose(raw, o);
    }, composer);
    box.Request(0, 1, 1);
    SceneMailbox::PumpTrace trace;
    assert(box.Pump(pack, box.Requested(), &trace) == Error::Ok && trace.published);
    auto lease = box.TakeReady();
    assert(lease.slot >= 0 && lease.pixels == (lease.slot ? b.data() : a.data()));
    std::vector<uint8_t> raw(L::kSrcBytes);
    assert(pack.Frame(1, raw.data(), raw.size()) == Error::Ok);
    std::vector<uint8_t> expect(L::kOutBytes);
    composer->Compose(raw.data(), expect.data());
    assert(std::equal(expect.begin(), expect.end(), lease.pixels));
    box.Release(lease);
    std::free(staging);
    delete composer;
    std::puts("PASS composer matches per-pixel layer reference (head over art, body only in "
              "window, art elsewhere); mailbox publishes composed frame from DMA staging");
}
