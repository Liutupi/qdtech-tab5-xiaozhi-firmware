#include "tab5_nes_video.h"

#include <atomic>
#include <cstring>

#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define TAG "Tab5NesVideo"

namespace tab5_nes_video {
namespace {

// Physical panel is 720x1280 portrait; LVGL shows it as 1280x720 with
// LV_DISPLAY_ROTATION_270 + software rotation. From lvgl_port_rotate_area():
// landscape (x, y) lands on physical (719 - y, x).
constexpr int kPhysW = 720;
constexpr int kPhysH = 1280;
constexpr int kNesW = 256;
constexpr int kNesH = 240;
constexpr size_t kFbBytes = size_t(kPhysW) * kPhysH * sizeof(uint16_t);

esp_lcd_panel_handle_t s_panel = nullptr;
lv_display_t* s_display = nullptr;
uint16_t* s_fb[2] = {};
int s_cur = 0;  // mirrors esp_lcd_dpi_panel_t::cur_fb_index (0 after init)
bool s_available = false;
std::atomic<bool> s_active{false};
std::atomic<bool> s_self_flip{false};
std::atomic<int> s_aspect{kAspectPixelPerfect};
int s_drawn_aspect = -1;
bool s_flip_pending = false;
SemaphoreHandle_t s_vsync = nullptr;

// Stats.
uint32_t s_presented = 0;
uint32_t s_dropped = 0;
int64_t s_blit_us = 0;
int64_t s_last_log_us = 0;

bool OnColorTransDone(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t*, void* ctx) {
    // Same as esp_lvgl_port's handler, except for our own zero-copy flips.
    if (s_self_flip.load(std::memory_order_relaxed)) {
        return false;
    }
    lv_display_flush_ready(static_cast<lv_display_t*>(ctx));
    return false;
}

bool OnRefreshDone(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t*, void*) {
    BaseType_t woken = pdFALSE;
    if (s_vsync) {
        xSemaphoreGiveFromISR(s_vsync, &woken);
    }
    return woken == pdTRUE;
}

void RegisterCallbacks(bool with_vsync) {
    esp_lcd_dpi_panel_event_callbacks_t cbs = {};
    cbs.on_color_trans_done = OnColorTransDone;
    // VSYNC IRQ only while a game owns the panel.
    cbs.on_refresh_done = with_vsync ? OnRefreshDone : nullptr;
    esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, s_display);
}

int ImageWidth(int aspect) { return aspect == kAspect4x3 ? 960 : 768; }

void FillRows(uint16_t* fb, int y0, int y1) {
    if (y1 > y0) {
        memset(fb + size_t(y0) * kPhysW, 0, size_t(y1 - y0) * kPhysW * sizeof(uint16_t));
    }
}

// Black out the landscape columns [0,1280) that are not covered by the current
// image in fb, but only inside the widest image area so the LVGL hint text
// in the outer bars stays.
void ClearImageArea(uint16_t* fb) {
    const int wide = ImageWidth(kAspect4x3);
    const int y0 = (kPhysH - wide) / 2;
    FillRows(fb, y0, y0 + wide);
    esp_cache_msync(fb + size_t(y0) * kPhysW, size_t(wide) * kPhysW * sizeof(uint16_t),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

// One pass: palette lookup, 3x vertical (landscape) / 3x or 3.75x horizontal
// nearest-neighbour scale, and the 90 degree rotation into portrait scan-out.
// Physical row py shows landscape column x = py; physical column px shows
// landscape row y = 719 - px, i.e. NES row 239 - px / 3.
__attribute__((optimize("O2"))) void Blit(uint16_t* fb, const uint8_t* const* lines,
                                          const uint16_t* pal, int aspect) {
    const int w = ImageWidth(aspect);
    const int x0 = (kPhysH - w) / 2;
    int j = 0;
    while (j < w) {
        const int nx = j * kNesW / w;
        int j_end = j + 1;
        while (j_end < w && j_end * kNesW / w == nx) {
            ++j_end;
        }
        uint16_t* row = fb + size_t(x0 + j) * kPhysW;
        uint32_t* dst = reinterpret_cast<uint32_t*>(row);
        // 2 NES pixels -> 6 physical pixels -> 3 words per iteration.
        for (int k = 0; k < kNesH / 2; ++k) {
            const uint32_t a = pal[lines[kNesH - 1 - 2 * k][nx]];
            const uint32_t b = pal[lines[kNesH - 2 - 2 * k][nx]];
            dst[0] = a | (a << 16);
            dst[1] = a | (b << 16);
            dst[2] = b | (b << 16);
            dst += 3;
        }
        for (int r = j + 1; r < j_end; ++r) {
            memcpy(fb + size_t(x0 + r) * kPhysW, row, kPhysW * sizeof(uint16_t));
        }
        j = j_end;
    }
}

}  // namespace

bool Init(esp_lcd_panel_handle_t panel, lv_display_t* display) {
    if (s_available) {
        return true;
    }
    if (!panel || !display) {
        return false;
    }
    void* fb0 = nullptr;
    void* fb1 = nullptr;
    if (esp_lcd_dpi_panel_get_frame_buffer(panel, 2, &fb0, &fb1) != ESP_OK || !fb0 || !fb1) {
        ESP_LOGW(TAG, "panel has <2 frame buffers; NES stays on the LVGL path");
        return false;
    }
    s_vsync = xSemaphoreCreateBinary();
    if (!s_vsync) {
        return false;
    }
    s_panel = panel;
    s_display = display;
    s_fb[0] = static_cast<uint16_t*>(fb0);
    s_fb[1] = static_cast<uint16_t*>(fb1);
    s_cur = 0;
    RegisterCallbacks(false);
    s_available = true;
    ESP_LOGI(TAG, "direct NES video ready fb0=%p fb1=%p", fb0, fb1);
    return true;
}

bool Available() { return s_available; }

bool Active() { return s_active.load(); }

void SetAspect(int aspect) {
    aspect = aspect == kAspect4x3 ? kAspect4x3 : kAspectPixelPerfect;
    if (s_aspect.exchange(aspect) != aspect) {
        ESP_LOGI(TAG, "aspect -> %s", aspect == kAspect4x3 ? "4:3" : "pixel-perfect 3x");
    }
}

int GetAspect() { return s_aspect.load(); }

bool Begin() {
    if (!s_available || s_active.load()) {
        return s_active.load();
    }
    // Let LVGL finish the play-mode screen (black stage + hint), then pause it.
    // Holding the lock guarantees no LVGL flush / DMA2D copy is in flight.
    if (!lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "begin: LVGL lock timeout, using LVGL path");
        return false;
    }
    lv_refr_now(s_display);
    lvgl_port_stop();
    lvgl_port_unlock();

    // Both buffers start as a copy of what LVGL drew; the NES image then only
    // overwrites the centre. DMA2D wrote the source, so drop stale cache first.
    uint16_t* front = s_fb[s_cur];
    uint16_t* back = s_fb[s_cur ^ 1];
    esp_cache_msync(front, kFbBytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    memcpy(back, front, kFbBytes);
    esp_cache_msync(back, kFbBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    ClearImageArea(front);
    ClearImageArea(back);

    xSemaphoreTake(s_vsync, 0);
    RegisterCallbacks(true);
    s_flip_pending = false;
    s_drawn_aspect = s_aspect.load();
    s_presented = 0;
    s_dropped = 0;
    s_blit_us = 0;
    s_last_log_us = esp_timer_get_time();
    s_active.store(true);
    ESP_LOGI(TAG, "begin: LVGL paused, direct scan-out (fb%d front)", s_cur);
    return true;
}

bool Present(const uint8_t* const* lines, const uint16_t* palette, uint16_t width,
             uint16_t height) {
    if (!s_active.load()) {
        return false;
    }
    if (!lines || !palette || width != kNesW || height != kNesH) {
        return true;
    }
    // The back buffer was on screen until the VSYNC after the last flip.
    // No VSYNC yet (panel ~57 Hz vs NES 60 Hz): drop this frame, never tear.
    if (s_flip_pending) {
        if (xSemaphoreTake(s_vsync, 0) != pdTRUE) {
            ++s_dropped;
            return true;
        }
        s_flip_pending = false;
    }

    const int64_t t0 = esp_timer_get_time();
    const int back_index = s_cur ^ 1;
    uint16_t* back = s_fb[back_index];
    const int aspect = s_aspect.load();
    if (aspect != s_drawn_aspect) {
        // Narrower image: wipe the strips the wider one left in both buffers.
        ClearImageArea(s_fb[0]);
        ClearImageArea(s_fb[1]);
        s_drawn_aspect = aspect;
    }
    Blit(back, lines, palette, aspect);

    const int w = ImageWidth(aspect);
    const int y0 = (kPhysH - w) / 2;
    xSemaphoreTake(s_vsync, 0);
    // Pointer inside a frame buffer: the driver only writes back the cache
    // for these rows and makes this buffer the scan-out one from next VSYNC.
    s_self_flip.store(true);
    esp_lcd_panel_draw_bitmap(s_panel, 0, y0, kPhysW, y0 + w, back + size_t(y0) * kPhysW);
    s_self_flip.store(false);
    s_cur = back_index;
    s_flip_pending = true;

    const int64_t t1 = esp_timer_get_time();
    s_blit_us += t1 - t0;
    ++s_presented;
    if (t1 - s_last_log_us >= 2000000) {
        ESP_LOGI(TAG, "shown=%lu dropped=%lu per 2s, blit avg=%lldus",
                 (unsigned long)s_presented, (unsigned long)s_dropped,
                 (long long)(s_presented ? s_blit_us / s_presented : 0));
        s_presented = 0;
        s_dropped = 0;
        s_blit_us = 0;
        s_last_log_us = t1;
    }
    return true;
}

void End() {
    if (!s_active.exchange(false)) {
        return;
    }
    RegisterCallbacks(false);
    // LVGL keeps drawing into the buffer that is on screen now (s_cur).
    if (lvgl_port_lock(1000)) {
        lvgl_port_resume();
        lv_obj_invalidate(lv_screen_active());
        lv_obj_invalidate(lv_layer_top());
        lvgl_port_unlock();
    } else {
        lvgl_port_resume();
    }
    ESP_LOGI(TAG, "end: LVGL resumed (fb%d front)", s_cur);
}

}  // namespace tab5_nes_video
