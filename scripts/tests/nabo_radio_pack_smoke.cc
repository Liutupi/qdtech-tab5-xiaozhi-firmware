// Opens a radio pack the way Tab5RadioStageReader does: with the home scene's
// pool sizes (571 KB slots, 395 KB encoded buffer) and a 451x395 canvas. Prints the decoded CRC of
// each frame for the caller.
#include <cassert>
#include <cstdio>
#include <vector>
#include "nabo_framepack.h"
#include "nabo_sd_host_source.h"
using namespace nabo_sd;

int main(int argc, char** argv) {
    assert(argc == 2);
    constexpr size_t kSlotBytes = 512 * 558 * 2, kEncodedBytes = 320 * 412 * 3 + 128;
    std::vector<uint8_t> encoded(kEncodedBytes), decoded(kSlotBytes);
    FileSource source(argv[1]);
    Pack pack;
    pack.SetDecodeBuffer(encoded.data(), encoded.size());
    assert(pack.Open(source, kSlotBytes, 300, true, 500, 600) == Error::Ok);
    assert(pack.Version() == 3);
    for (unsigned i = 0; i < pack.Count(); ++i) {
        const Entry e = pack.At(i);
        assert(e.x + e.width <= 451 && e.y + e.height <= 395);
        assert(e.bytes <= kEncodedBytes);
        assert(pack.Frame(i, decoded.data(), decoded.size()) == Error::Ok);
        std::printf("%u %u %u %u %u %08x\n", e.at_ms, e.x, e.y, e.width, e.height,
                    Crc(decoded.data(), size_t(e.width) * e.height * 3));
    }
    return 0;
}
