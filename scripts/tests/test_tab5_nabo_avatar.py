"""Check the small SD portrait's sampling, alpha edges and frame bounds."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/qdtech/tab5"


class NaboAvatarTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ host compiler required")
    def test_scaled_frames_preserve_alpha_and_cropped_bounds(self):
        harness = r'''
#include "nabo_avatar_frame.h"
#include <cassert>
#include <vector>
using namespace nabo_avatar;

int main() {
    constexpr int w = 320, h = 348;
    constexpr size_t pixels = w * h;
    std::vector<uint16_t> storage((pixels * 3 + 1) / 2, 0xffff);
    auto* raw = reinterpret_cast<uint8_t*>(storage.data());
    auto* alpha = raw + pixels * 2;
    std::vector<uint16_t> output(kWidth * kHeight + 2, 0x1234);
    auto* out = output.data() + 1;
    const nabo_sd::Entry full{0, 0, 0, 0, 0, 0, w, h};

    assert(Compose(full, raw, w, h, out));
    for (int i = 0; i < kWidth * kHeight; ++i) assert(out[i] == 0xffff);
    std::fill_n(alpha, pixels, 0);
    assert(Compose(full, raw, w, h, out));
    for (int i = 0; i < kWidth * kHeight; ++i) assert(out[i] == kBackground);

    // Half-alpha red blends with the card rather than a black fringe.
    std::fill_n(storage.data(), pixels, 0xf800);
    std::fill_n(alpha, pixels, 128);
    assert(Compose(full, raw, w, h, out));
    for (int i = 0; i < kWidth * kHeight; ++i) assert(out[i] == 0x8061);

    // Two-by-two black/white squares become an even grey, not flickering pixels.
    std::fill_n(alpha, pixels, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            storage[y * w + x] = (x + y) % 2 ? 0xffff : 0;
    assert(Compose(full, raw, w, h, out));
    for (int i = 0; i < kWidth * kHeight; ++i) assert(out[i] == 0x7bef);

    // Cropped frames keep their canvas position; transparent margins stay clear.
    const nabo_sd::Entry crop{0, 0, 0, 0, 100, 80, 40, 40};
    std::vector<uint16_t> patch(40 * 40 * 3 / 2, 0xffff);
    assert(Compose(crop, reinterpret_cast<const uint8_t*>(patch.data()), w, h, out));
    assert(out[45 * kWidth + 55] == 0xffff);
    assert(out[0] == kBackground && out[39 * kWidth + 50] == kBackground);
    assert(out[60 * kWidth + 55] == kBackground);

    // Legacy 320x412 portraits fit without stretching or clipping their edges.
    std::vector<uint16_t> legacy(320 * 412 * 3 / 2, 0xffff);
    const nabo_sd::Entry old{0, 0, 0, 0, 0, 0, 320, 412};
    assert(Compose(old, reinterpret_cast<const uint8_t*>(legacy.data()), 320, 412, out));
    assert(out[0] == kBackground && out[kWidth - 1] == kBackground);
    assert(out[87 * kWidth + 80] == 0xffff);
    assert(!Compose(full, raw, 319, h, out));
    assert(!Compose(full, nullptr, w, h, out));
    assert(!Compose(full, raw, w, 0, out));
    assert(output.front() == 0x1234 && output.back() == 0x1234);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "avatar.cc"
            binary = Path(directory) / "avatar"
            source.write_text(harness)
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(BOARD), str(source),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
