"""The real direct blit must allow idle scheduling even when DMA finishes immediately."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "main/boards/qdtech/tab5/tab5_native_display.h"


def harness():
    source = SOURCE.read_text()
    method = source[source.index("    bool BlitRotated("):
                    source.index("    bool TickSdScene(")]
    return r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

constexpr int LV_DISPLAY_ROTATION_270 = 270, ESP_OK = 0;
constexpr int MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
struct lv_area_t { int x1, y1, x2, y2; };
int lv_area_get_width(const lv_area_t* a) { return a->x2 - a->x1 + 1; }
int lv_area_get_height(const lv_area_t* a) { return a->y2 - a->y1 + 1; }
struct Display { bool flushing = false; int rotation = 270; };
int lv_display_get_rotation(Display* d) { return d->rotation; }
int lv_display_get_vertical_resolution(Display*) { return 720; }
int64_t now_us = 0;
int idle_ticks = 0, draws = 0, dma_ticks = 0;
bool draw_fails = false;
Display* pending = nullptr;
int64_t esp_timer_get_time() { return now_us; }
constexpr int pdMS_TO_TICKS(int ms) { return ms; }
void vTaskDelay(int ticks) {
    idle_ticks += ticks;
    now_us += ticks * 1000;
    if (pending && dma_ticks > 0) {
        dma_ticks -= ticks;
        if (dma_ticks <= 0) pending->flushing = false;
    }
}
void* heap_caps_aligned_alloc(size_t alignment, size_t size, int) {
    void* p = nullptr;
    return posix_memalign(&p, alignment, size) == 0 ? p : nullptr;
}
int esp_lcd_panel_draw_bitmap(Display* panel, int, int, int, int, const void*) {
    ++draws;
    pending = panel;
    if (draw_fails) return -1;
    if (dma_ticks == 0) panel->flushing = false;
    return ESP_OK;
}
#define ESP_LOGE(...) ((void)0)
namespace nabo_sd {
void Rotate270Rgb565(const uint16_t*, uint16_t*, int, int, int, int) {}
}
struct QdtechTab5Display {
    Display state;
    Display* display_ = &state;
    Display* panel_ = &state;
    uint8_t* sd_rotated_ = nullptr;
    size_t sd_rotated_bytes_ = 0;
    bool sd_direct_disabled_ = false;
    ~QdtechTab5Display() { std::free(sd_rotated_); }
''' + method + r'''
};

int main() {
    QdtechTab5Display display;
    const lv_area_t area{0, 0, 7, 7};
    const uint8_t pixels[128] = {};
    // A stream of immediate completions must still unblock the idle task at
    // least once per frame; otherwise high-priority LVGL can starve IDLE0.
    for (int i = 0; i < 40; ++i) {
        const int before = idle_ticks;
        assert(display.BlitRotated(area, pixels));
        assert(idle_ticks - before >= 5);
        assert(!display.state.flushing);
    }
    dma_ticks = 12;
    const int before = idle_ticks;
    assert(display.BlitRotated(area, pixels));
    assert(idle_ticks - before == 15 && !display.state.flushing);
    // A lost completion still respects the existing 100 ms bound and stops
    // future direct writes from reusing the in-flight frame buffer.
    dma_ticks = -1;
    const auto start = now_us;
    assert(!display.BlitRotated(area, pixels));
    assert(now_us - start == 100000 && display.sd_direct_disabled_);
    const int draws_before = draws;
    assert(!display.BlitRotated(area, pixels));
    assert(draws == draws_before);
    QdtechTab5Display failed;
    draw_fails = true;
    assert(!failed.BlitRotated(area, pixels));
    assert(!failed.state.flushing && !failed.sd_direct_disabled_);
}
'''


@unittest.skipUnless(shutil.which("c++"), "C++ host compiler required")
class DirectBlitYieldTests(unittest.TestCase):
    def test_immediate_delayed_and_missing_dma_completion(self):
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "direct_blit.cc"
            binary = Path(directory) / "direct_blit"
            cpp.write_text(harness())
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
