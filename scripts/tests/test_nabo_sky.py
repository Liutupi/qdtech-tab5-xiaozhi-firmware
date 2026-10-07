"""Sky behind Nabo: selection, moon phase, deterministic soft-edged base, fx placement."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

BOARD = Path(__file__).resolve().parents[2] / "main/boards/qdtech/tab5"

SMOKE = r'''
#include <cassert>
#include <cmath>
#include <memory>
#include <cstdio>
#include <vector>
#include "nabo_sky.h"
using namespace nabo_sky;
int main() {
    const int W = 512, H = 558;
    assert(PeriodAt(12 * 60) == Period::Day && PeriodAt(2 * 60) == Period::Night);
    assert(PeriodAt(18 * 60 + 15) == Period::Dusk && PeriodAt(6 * 60) == Period::Dusk);
    assert(PeriodAt(23 * 60) == Period::Night && PeriodAt(19 * 60 + 30) == Period::Night);
    assert(PeriodAt(17 * 60, 6 * 60, 17 * 60 + 30) == Period::Dusk);  // real sunset used
    assert(WeatherFromWmo(0) == Weather::Clear && WeatherFromWmo(3) == Weather::Cloudy &&
           WeatherFromWmo(45) == Weather::Cloudy && WeatherFromWmo(63) == Weather::Rain &&
           WeatherFromWmo(95) == Weather::Rain && WeatherFromWmo(73) == Weather::Snow &&
           WeatherFromWmo(-1) == Weather::Clear);
    // Moon phase: 2024-01-25 17:54 UTC full moon, 2024-01-11 11:57 UTC new moon.
    assert(std::abs(MoonAge(1706205240) - 14.77) < 1.0);
    const double new_age = MoonAge(1704974220);
    assert(new_age < 1.0 || new_age > 28.5);
    const int full_day = SelectSky(12 * 60, 360, 1095, 0, 1706205240).moon_day;
    assert(full_day >= 14 && full_day <= 16);
    // Sun and moon stay well right of Nabo's cap (which ends near x = 440).
    assert(kOrbX - 18 > 440 && kOrbY - 18 > 20);
    std::vector<uint16_t> a(W * H), b(W * H);
    auto fx = std::make_unique<SkyFx>();
    const uint16_t page = To565(Hex(kPage));
    for (int p = 0; p < 3; ++p)
        for (int w = 0; w < 4; ++w) {
            const Sky sky{Period(p), Weather(w), uint8_t(p * 5 + w * 2 + 3)};
            RenderSkyBase(sky, a.data(), W, H);
            RenderSkyBase(sky, b.data(), W, H);
            assert(a == b);
            // No frame: the outer edge is the page colour itself.
            for (int x = 0; x < W; ++x)
                assert(a[x] == page && a[(H - 1) * W + x] == page);
            for (int y = 0; y < H; ++y)
                assert(a[y * W] == page && a[y * W + W - 1] == page);
            fx->Reset(sky);
            std::vector<uint16_t> f = a;
            fx->Draw(1234, f.data(), W, H, 0, [](int, int) { return 255; });
            assert(f == a);  // never over an opaque Nabo
            for (uint32_t t : {0u, 4321u, 30000u, 599999u})
                fx->Draw(t, f.data(), W, H, 0, [](int, int) { return 0; });
            for (int y = kFxBottom; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    assert(f[y * W + x] == a[y * W + x]);  // never below the window line
            // Nothing drawn where composed frames end (x < 39 or x > 472): no cut-off edge.
            for (int y = 0; y < H; ++y)
                for (int x = 0; x <= 39; ++x)
                    assert(f[y * W + x] == a[y * W + x] && f[y * W + W - 1 - x] == a[y * W + W - 1 - x]);
            // Output frames start at the head clip (x = 39) and are 434 wide.
            std::vector<uint16_t> part(434 * H, 0);
            fx->Draw(4321, part.data(), 434, H, 39, [](int, int) { return 0; });
            if (w != int(Weather::Clear)) {  // clouds drift
                std::vector<uint16_t> c0 = a, c1 = a;
                fx->Draw(0, c0.data(), W, H, 0, [](int, int) { return 0; });
                fx->Draw(20000, c1.data(), W, H, 0, [](int, int) { return 0; });
                assert(c0 != c1);
            }
        }
    std::puts("PASS sky selection, deterministic base, moon phase, soft edge, drifting clouds, fx never over Nabo or below the window");
}
'''


class NaboSkyTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_sky(self):
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "sky.cc"
            cpp.write_text(SMOKE)
            binary = Path(directory) / "sky"
            subprocess.run(["c++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(BOARD), str(cpp),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
