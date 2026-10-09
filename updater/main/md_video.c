#include "md_video.h"

#include <string.h>

#include "esp_cache.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char* TAG = "md_video";

#define SCALE 3
#define FB_BYTES ((size_t)MD_PANEL_W * MD_PANEL_H * 2)

static md_board_t* s_board;
static SemaphoreHandle_t s_vsync;
static int s_cur;  // index of the buffer being scanned out
static bool s_flip_pending;
static uint32_t s_shown, s_skipped;
static int64_t s_blit_us, s_last_log;
static int s_last_w, s_last_h;

static bool on_refresh_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t* edata, void* ctx) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_vsync, &woken);
    return woken == pdTRUE;
}

void md_video_clear(uint16_t color) {
    for (int b = 0; b < 2; ++b) {
        uint16_t* fb = s_board->fb[b];
        for (size_t i = 0; i < (size_t)MD_PANEL_W * MD_PANEL_H; ++i)
            fb[i] = color;
        esp_cache_msync(fb, FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
}

void md_video_init(md_board_t* board) {
    s_board = board;
    s_vsync = xSemaphoreCreateBinary();
    esp_lcd_dpi_panel_event_callbacks_t cbs = {.on_refresh_done = on_refresh_done};
    esp_lcd_dpi_panel_register_event_callbacks(board->panel, &cbs, NULL);
    s_cur = 0;
    md_video_clear(0);
    s_last_log = esp_timer_get_time();
}

// Landscape (x, y) is physical (719 - y, x): physical row py shows landscape column x,
// physical column px shows landscape row 719 - px.
__attribute__((optimize("O3"))) static void blit(uint16_t* fb, const uint8_t* lines, int w, int h,
                                                 const uint16_t* pal) {
    const int out_w = w * SCALE, out_h = h * SCALE;
    const int x0 = (MD_PANEL_H - out_w) / 2;  // first physical row of the image
    const int y0 = (MD_PANEL_W - out_h) / 2;  // landscape top margin
    // Physical columns [719 - y0 - out_h + 1, 719 - y0] hold the image; source row sy
    // lands at px = 719 - y0 - 3 sy - k, i.e. reversed.
    const int px_first = MD_PANEL_W - y0 - out_h;
    for (int sx = 0; sx < w; ++sx) {
        uint16_t* row = fb + (size_t)(x0 + sx * SCALE) * MD_PANEL_W;
        memset(row, 0, (size_t)px_first * 2);
        uint16_t* dst = row + px_first;
        for (int sy = h - 1; sy >= 0; --sy) {
            const uint16_t c = pal[lines[sy * 320 + sx]];
            dst[0] = c;
            dst[1] = c;
            dst[2] = c;
            dst += 3;
        }
        memset(dst, 0, (size_t)(MD_PANEL_W - px_first - out_h) * 2);
        memcpy(row + MD_PANEL_W, row, MD_PANEL_W * 2);
        memcpy(row + 2 * MD_PANEL_W, row, MD_PANEL_W * 2);
    }
}

bool md_video_present(const uint8_t* lines, int width, int height, const uint16_t* palette) {
    if (s_flip_pending) {
        if (xSemaphoreTake(s_vsync, 0) != pdTRUE) {
            ++s_skipped;
            return false;
        }
        s_flip_pending = false;
    }
    const int64_t t0 = esp_timer_get_time();
    const int back = s_cur ^ 1;
    uint16_t* fb = s_board->fb[back];
    if (width != s_last_w || height != s_last_h) {
        // Mode change (H32/H40, NTSC/PAL): clear the old image area in both buffers.
        for (int b = 0; b < 2; ++b) {
            memset(s_board->fb[b], 0, FB_BYTES);
            esp_cache_msync(s_board->fb[b], FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        }
        s_last_w = width;
        s_last_h = height;
    }
    blit(fb, lines, width, height, palette);
    const int x0 = (MD_PANEL_H - width * SCALE) / 2;
    xSemaphoreTake(s_vsync, 0);
    // Passing the DPI frame buffer itself makes the driver flip to it (no copy).
    esp_lcd_panel_draw_bitmap(s_board->panel, 0, x0, MD_PANEL_W, x0 + width * SCALE,
                              fb + (size_t)x0 * MD_PANEL_W);
    s_cur = back;
    s_flip_pending = true;
    const int64_t t1 = esp_timer_get_time();
    s_blit_us += t1 - t0;
    ++s_shown;
    if (t1 - s_last_log >= 2000000) {
        ESP_LOGI(TAG, "shown=%lu skipped=%lu per 2 s, blit avg=%lld us", (unsigned long)s_shown,
                 (unsigned long)s_skipped, (long long)(s_shown ? s_blit_us / s_shown : 0));
        s_shown = s_skipped = 0;
        s_blit_us = 0;
        s_last_log = t1;
    }
    return true;
}
