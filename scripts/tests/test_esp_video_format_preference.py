"""Exercise EspVideo's real V4L2 format selection with enumerated formats."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "main/boards/common/esp_video.cc"


def selection_harness(source: str, ppa: bool) -> str:
    start = source.index("    struct v4l2_fmtdesc fmtdesc = {};")
    selection = source[
        start:source.index("#if CONFIG_XIAOZHI_CAMERA_MIRROR_CONFIGURED", start)
    ]
    defines = "#define CONFIG_XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE 1\n"
    if ppa:
        defines += "#define CONFIG_SOC_PPA_SUPPORTED 1\n"
    return defines + r'''
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define CAM_PRINT_FOURCC(...) ((void)0)
#define TAG "EspVideo"

constexpr int V4L2_BUF_TYPE_VIDEO_CAPTURE = 1;
constexpr int VIDIOC_ENUM_FMT = 2;
constexpr int VIDIOC_S_FMT = 3;
constexpr uint32_t V4L2_PIX_FMT_RGB24 = 10;
constexpr uint32_t V4L2_PIX_FMT_RGB565 = 11;
constexpr uint32_t V4L2_PIX_FMT_RGB565X = 12;
constexpr uint32_t V4L2_PIX_FMT_YUYV = 13;
constexpr uint32_t V4L2_PIX_FMT_UYVY = 14;
constexpr uint32_t V4L2_PIX_FMT_YUV422P = 15;
constexpr uint32_t V4L2_PIX_FMT_GREY = 16;

struct v4l2_fmtdesc {
    int type = 0;
    uint32_t index = 0;
    uint32_t pixelformat = 0;
    char description[16] = {};
};
struct v4l2_format {
    int type = 0;
    struct {
        struct {
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t pixelformat = 0;
        } pix;
    } fmt;
};

std::vector<uint32_t> available;
std::vector<uint32_t> attempted;
bool reject_rgb565 = false;
bool coerce_rgb565 = false;
int closed = 0;

int ioctl(int, int request, v4l2_fmtdesc* desc) {
    if (request != VIDIOC_ENUM_FMT || desc->index >= available.size()) return -1;
    desc->pixelformat = available[desc->index];
    return 0;
}
int ioctl(int, int request, v4l2_format* format) {
    if (request != VIDIOC_S_FMT) return -1;
    attempted.push_back(format->fmt.pix.pixelformat);
    if (reject_rgb565 && format->fmt.pix.pixelformat == V4L2_PIX_FMT_RGB565) {
        errno = EINVAL;
        return -1;
    }
    if (coerce_rgb565 && format->fmt.pix.pixelformat == V4L2_PIX_FMT_RGB565)
        format->fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
    return 0;
}
int close(int) { ++closed; return 0; }

enum class PixelFormatPreference { Default, PreferRgb565 };
uint32_t selected = 0;
void select_format(PixelFormatPreference pixel_format_preference) {
    int video_fd_ = 1;
    uint32_t sensor_width_ = 1280;
    uint32_t sensor_height_ = 720;
    uint32_t sensor_format_ = 0;
    v4l2_format setformat = {};
    setformat.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    setformat.fmt.pix.width = sensor_width_;
    setformat.fmt.pix.height = sensor_height_;
''' + selection + r'''
    selected = sensor_format_;
}

bool run(const std::vector<uint32_t>& formats, PixelFormatPreference preference,
         uint32_t expected, const std::vector<uint32_t>& expected_attempts,
         bool reject = false, bool coerce = false) {
    available = formats;
    attempted.clear();
    reject_rgb565 = reject;
    coerce_rgb565 = coerce;
    closed = 0;
    selected = 0;
    select_format(preference);
    const bool ok = selected == expected && attempted == expected_attempts &&
                    closed == (expected ? 0 : 1);
    if (!ok) {
        std::fprintf(stderr, "selected=%u expected=%u closed=%d attempts=", selected,
                     expected, closed);
        for (uint32_t format : attempted) std::fprintf(stderr, "%u,", format);
        std::fprintf(stderr, "\n");
    }
    return ok;
}

int main() {
    using P = PixelFormatPreference;
#if defined(CONFIG_SOC_PPA_SUPPORTED)
    if (!run({V4L2_PIX_FMT_RGB565, V4L2_PIX_FMT_RGB24}, P::Default,
             V4L2_PIX_FMT_RGB24, {V4L2_PIX_FMT_RGB24})) return 1;
    if (!run({V4L2_PIX_FMT_RGB24, V4L2_PIX_FMT_RGB565}, P::PreferRgb565,
             V4L2_PIX_FMT_RGB565, {V4L2_PIX_FMT_RGB565})) return 2;
    if (!run({V4L2_PIX_FMT_RGB24}, P::PreferRgb565,
             V4L2_PIX_FMT_RGB24, {V4L2_PIX_FMT_RGB24})) return 3;
    if (!run({V4L2_PIX_FMT_RGB565X, V4L2_PIX_FMT_RGB24}, P::PreferRgb565,
             V4L2_PIX_FMT_RGB24, {V4L2_PIX_FMT_RGB24})) return 4;
    if (!run({V4L2_PIX_FMT_RGB565, V4L2_PIX_FMT_RGB24}, P::PreferRgb565,
             V4L2_PIX_FMT_RGB24,
             {V4L2_PIX_FMT_RGB565, V4L2_PIX_FMT_RGB24}, true)) return 5;
    if (!run({V4L2_PIX_FMT_RGB565}, P::PreferRgb565,
             0, {V4L2_PIX_FMT_RGB565}, true)) return 6;
    if (!run({V4L2_PIX_FMT_RGB565, V4L2_PIX_FMT_RGB24}, P::PreferRgb565,
             V4L2_PIX_FMT_RGB24, {V4L2_PIX_FMT_RGB565}, false, true)) return 10;
#else
    if (!run({V4L2_PIX_FMT_RGB24, V4L2_PIX_FMT_YUYV}, P::Default,
             V4L2_PIX_FMT_YUYV, {V4L2_PIX_FMT_YUYV})) return 7;
    if (!run({V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_RGB24, V4L2_PIX_FMT_RGB565},
             P::PreferRgb565, V4L2_PIX_FMT_RGB565,
             {V4L2_PIX_FMT_RGB565})) return 8;
    if (!run({V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_RGB24}, P::PreferRgb565,
             V4L2_PIX_FMT_RGB24, {V4L2_PIX_FMT_RGB24})) return 9;
#endif
    std::puts("Camera format selection and RGB24 fallback passed.");
}
'''


class CameraFormatPreferenceTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ compiler required")
    def test_rgb565_preference_and_rgb24_fallback(self):
        source = SOURCE.read_text()
        with tempfile.TemporaryDirectory() as directory:
            for ppa in (False, True):
                test = Path(directory) / f"camera-format-{ppa}.cc"
                binary = Path(directory) / f"camera-format-{ppa}"
                test.write_text(selection_harness(source, ppa))
                subprocess.run(
                    ["c++", "-std=c++17", str(test), "-o", str(binary)],
                    check=True, capture_output=True, text=True,
                )
                result = subprocess.run(
                    [str(binary)], capture_output=True, text=True, timeout=10,
                )
                self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
