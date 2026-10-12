#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "nabo_framepack.h"
#include "nabo_sd_host_source.h"
#include "nabo_wide_composer.h"
using namespace nabo_sd;
int main(int argc, char** argv) {
    uint8_t out[32]{};
    const uint8_t literal[] = {0x30, 'a', 'b', 'c'};
    assert(DecodeLz4(literal, sizeof(literal), out, 3));
    assert(!DecodeLz4(literal, sizeof(literal) - 1, out, 3));
    assert(!DecodeLz4(literal, sizeof(literal), out, 2));
    const uint8_t overlap[] = {0x15, 'x', 1, 0, 0x50, 'a', 'b', 'c', 'd', 'e'};
    assert(DecodeLz4(overlap, sizeof(overlap), out, 15));
    for (int i = 0; i < 10; ++i)
        assert(out[i] == 'x');
    const uint8_t bad_offset[] = {0x10, 'a', 0, 0};
    assert(!DecodeLz4(bad_offset, sizeof(bad_offset), out, 8));
    const uint8_t bad_extension[] = {0xf0, 255};
    assert(!DecodeLz4(bad_extension, sizeof(bad_extension), out, 8));
    assert(argc >= 2);
    std::vector<uint8_t> encoded(512 * 558 * 3), decoded(encoded.size());
    unsigned frames = 0;
    for (int file = 1; file < argc; ++file) {
        FileSource source(argv[file]);
        Pack pack;
        assert(pack.Open(source, decoded.size(), 10000, false) == Error::Format);
        assert(pack.Open(source, decoded.size(), 10000, true) == Error::Ok);
        assert(pack.Version() == 3);
        assert(pack.Frame(0, decoded.data(), decoded.size()) == Error::Capacity);
        pack.SetDecodeBuffer(encoded.data(), encoded.size());
        uint32_t decoded_crc = ~0u;
        for (unsigned i = 0; i < pack.Count(); ++i) {
            assert(pack.Frame(i, decoded.data(), decoded.size()) == Error::Ok);
            assert(pack.At(i).offset % 128 == 0);
            decoded_crc = CrcStep(decoded_crc, decoded.data(), pack.MaxPayload());
            ++frames;
        }
        std::printf("DECODED %s %08x\n", argv[file], ~decoded_crc);
        pack.SetVerifyOnce(true);
        assert(pack.Frame(0, decoded.data(), decoded.size()) == Error::Ok);
        const auto crc = Crc(decoded.data(), pack.MaxPayload());
        assert(pack.Frame(0, decoded.data(), decoded.size()) == Error::Ok);
        assert(Crc(decoded.data(), pack.MaxPayload()) == crc);
        assert(pack.Frame(0, decoded.data(), pack.MaxPayload() - 1) == Error::Capacity);
        std::atomic<uint32_t> epoch{2};
        assert(pack.Frame(0, decoded.data(), decoded.size(), {&epoch, 1}) == Error::Cancelled);
    }
    // The new compositor preserves B side posts/sill, alpha and sky replacement.
    using L = WideLayout;
    std::vector<uint16_t> bg(L::kOutW * L::kOutH, 0x1234);
    std::vector<uint8_t> mask(bg.size()), rgba(L::kSrcBytes, 255), dst(L::kOutBytes);
    mask[400 * L::kOutW + 11] = 1;
    WideComposer compositor(bg.data(), mask.data());
    compositor.SetLegacy(false);
    compositor.Compose(rgba.data(), dst.data());
    const auto* pixels = reinterpret_cast<const uint16_t*>(dst.data());
    for (int y = 0; y < L::kOutH; ++y)
        for (int x = 0; x < L::kOutW; ++x) {
            const bool body = y < 539 && x >= 12 && x < 490;
            const bool visible = (y < 423 || body) && !mask[y * L::kOutW + x];
            assert(pixels[y * L::kOutW + x] == (visible ? 0xffff : 0x1234));
        }
    std::printf(
        "PASS bounded LZ4, %u real compressed frames, alignment, CRC/decode/cancellation/capacity, "
        "B compositor\n",
        frames);
}
