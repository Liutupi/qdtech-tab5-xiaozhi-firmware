"""CMA station observations and local sunrise/sunset for the Tab5 weather."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/qdtech/tab5"
CJSON = ROOT / "managed_components/espressif__cjson/cJSON"

SMOKE = r'''
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "tab5_weather_obs.h"
using namespace tab5_weather;
int main() {
    // Real response shape from www.nmc.cn/rest/real/YIWau (2026-10-12).
    const char* clear = R"({"station":{"code":"YIWau","city":"中山"},"publish_time":"2026-10-12 15:20",
      "weather":{"temperature":31.0,"humidity":57.0,"rain":0.0,"info":"晴","img":"0","feelst":34.3},
      "warn":{"alert":"9999","signaltype":"9999"}})";
    Observation o;
    assert(ParseCmaReal(clear, &o));
    assert(o.valid && o.wmo_code == 0 && std::fabs(o.temperature - 31.0) < 1e-9 && o.humidity == 57);
    assert(std::strcmp(o.info, "晴") == 0 && std::strcmp(o.time, "15:20") == 0 && !o.warning);

    const char* storm = R"({"publish_time":"2026-07-01 08:05","weather":{"temperature":"27.4","humidity":"95",
      "rain":"12.5","info":"雷阵雨","img":"4"},"warn":{"alert":"中山市暴雨黄色预警信号"}})";
    assert(ParseCmaReal(storm, &o) && o.wmo_code == 95 && o.rain == 12.5 && o.warning);
    assert(std::strstr(o.warning_text, "暴雨") != nullptr);

    // No data (9999) or junk -> invalid, so the caller falls back to Open-Meteo.
    assert(!ParseCmaReal(R"({"weather":{"temperature":9999,"img":"9999"}})", &o) && !o.valid);
    assert(!ParseCmaReal("not json", &o));
    assert(CmaToWmo(1) == 2 && CmaToWmo(2) == 3 && CmaToWmo(7) == 61 && CmaToWmo(9) == 65 &&
           CmaToWmo(14) == 71 && CmaToWmo(18) == 45 && CmaToWmo(53) == 45 && CmaToWmo(9999) == -1);

    // Zhongshan, 2026-10-12: Open-Meteo gives sunrise 06:21, sunset 18:03.
    SunTimes s = ComputeSun(22.5176, 113.3928, 2026, 10, 12, 8);
    assert(std::abs(s.sunrise_min - (6 * 60 + 19)) <= 3 && std::abs(s.sunset_min - (18 * 60 + 3)) <= 3);
    // Mid-summer and mid-winter stay plausible.
    SunTimes june = ComputeSun(22.5176, 113.3928, 2026, 6, 21, 8);
    SunTimes dec = ComputeSun(22.5176, 113.3928, 2026, 12, 21, 8);
    assert(june.sunrise_min < dec.sunrise_min && june.sunset_min > dec.sunset_min);
    assert(ComputeSun(80, 0, 2026, 6, 21, 0).sunrise_min == -1);  // polar day
    std::printf("PASS weather obs: sunrise %02d:%02d sunset %02d:%02d\n", s.sunrise_min / 60, s.sunrise_min % 60,
                s.sunset_min / 60, s.sunset_min % 60);
}
'''


class WeatherObsTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++") and shutil.which("cc"), "C/C++ compiler required")
    def test_weather_obs(self):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            subprocess.run(["cc", "-c", str(CJSON / "cJSON.c"), "-o", str(temp / "cjson.o")], check=True)
            (temp / "w.cc").write_text(SMOKE)
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                            "-I", str(BOARD), "-I", str(CJSON), str(temp / "w.cc"), str(temp / "cjson.o"),
                            "-o", str(temp / "w")], check=True)
            result = subprocess.run([str(temp / "w")], check=True, capture_output=True, text=True)
            print(result.stdout.strip())


if __name__ == "__main__":
    unittest.main()
