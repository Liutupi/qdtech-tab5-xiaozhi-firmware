#include "md_video.h"

#include <string.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char* TAG = "md_video";

#define SCALE 3
#define FB_BYTES ((size_t)MD_PANEL_W * MD_PANEL_H * 2)
#define RGB_BYTES (320 * 240 * 2)

// Two-core pipeline: the emulator (core 0) only expands palette indices into one of two
// small RGB565 pictures (~1 ms); a presenter task on core 1 waits for VSYNC, has the PPA
// scale it 3x and rotate it into the back scan-out buffer (~9 ms) and flips. The newest
// picture wins: one the presenter has not picked up yet is overwritten.
static md_board_t* s_board;
static SemaphoreHandle_t s_vsync;
static TaskHandle_t s_presenter;
static ppa_client_handle_t s_ppa;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint16_t* s_rgb[2];
static int s_rgb_w[2], s_rgb_h[2];
static int s_ready = -1;  // picture waiting for the presenter
static int s_busy = -1;   // picture the presenter is reading
static int s_cur;         // scan-out buffer on screen
static bool s_flip_pending;
static int s_last_w, s_last_h;
static uint32_t s_shown, s_replaced;
static int64_t s_ppa_us, s_last_log;

static bool on_refresh_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t* edata, void* ctx) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_vsync, &woken);
    return woken == pdTRUE;
}

// Landscape sidebar button stays outside the 3x game image in H32/H40 modes.
static void draw_exit(uint16_t* fb) {
    static const uint8_t glyph[4][7] = {{31, 16, 16, 30, 16, 16, 31},
                                        {17, 17, 10, 4, 10, 17, 17},
                                        {31, 4, 4, 4, 4, 4, 31},
                                        {31, 4, 4, 4, 4, 4, 4}};
    for (int y = 36; y < 96; ++y) {
        for (int x = 24; x < 136; ++x) {
            const bool border = x < 27 || x >= 133 || y < 39 || y >= 93;
            uint16_t color = border ? 0xffff : 0x20e6;
            const int gx = (x - 34) / 4, gy = (y - 51) / 4;
            if (x >= 34 && y >= 51 && gy < 7 && gx >= 0 && gx < 24 && gx % 6 < 5 &&
                (glyph[gx / 6][gy] & (1u << (4 - gx % 6))))
                color = 0xffff;
            fb[(size_t)x * MD_PANEL_W + MD_PANEL_W - 1 - y] = color;
        }
    }
}

void md_video_clear(uint16_t color) {
    for (int b = 0; b < 2; ++b) {
        uint16_t* fb = s_board->fb[b];
        for (size_t i = 0; i < (size_t)MD_PANEL_W * MD_PANEL_H; ++i)
            fb[i] = color;
        draw_exit(fb);
        esp_cache_msync(fb, FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
}

static void present_one(int i) {
    const int w = s_rgb_w[i], h = s_rgb_h[i];
    if (s_flip_pending) {
        // The previous flip happens at VSYNC; until then the old front is still scanned out.
        xSemaphoreTake(s_vsync, pdMS_TO_TICKS(50));
        s_flip_pending = false;
    }
    if (w != s_last_w || h != s_last_h) {
        // Mode change (H32/H40, NTSC/PAL): clear the old image area in both buffers.
        for (int b = 0; b < 2; ++b) {
            memset(s_board->fb[b], 0, FB_BYTES);
            draw_exit(s_board->fb[b]);
            esp_cache_msync(s_board->fb[b], FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        }
        s_last_w = w;
        s_last_h = h;
    }
    const int back = s_cur ^ 1;
    uint16_t* fb = s_board->fb[back];
    const int64_t t0 = esp_timer_get_time();
    // Scale 3x and rotate 90 degrees clockwise (270 CCW): landscape (x, y) -> physical (719 - y, x).
    ppa_srm_oper_config_t op = {
        .in = {.buffer = s_rgb[i], .pic_w = w, .pic_h = h, .block_w = w, .block_h = h,
               .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = {.buffer = fb, .buffer_size = FB_BYTES, .pic_w = MD_PANEL_W, .pic_h = MD_PANEL_H,
                .block_offset_x = (MD_PANEL_W - h * SCALE) / 2,
                .block_offset_y = (MD_PANEL_H - w * SCALE) / 2,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,
        .scale_x = SCALE,
        .scale_y = SCALE,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_scale_rotate_mirror(s_ppa, &op) != ESP_OK)
        return;
    const int64_t t1 = esp_timer_get_time();
    const int x0 = (MD_PANEL_H - w * SCALE) / 2;
    xSemaphoreTake(s_vsync, 0);
    // Passing the DPI frame buffer itself makes the driver flip to it (no copy).
    esp_lcd_panel_draw_bitmap(s_board->panel, 0, x0, MD_PANEL_W, x0 + w * SCALE, fb + (size_t)x0 * MD_PANEL_W);
    s_cur = back;
    s_flip_pending = true;
    s_ppa_us += t1 - t0;
    ++s_shown;
}

static void presenter_task(void* arg) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        portENTER_CRITICAL(&s_lock);
        const int i = s_ready;
        s_busy = i;
        s_ready = -1;
        portEXIT_CRITICAL(&s_lock);
        if (i >= 0)
            present_one(i);
        portENTER_CRITICAL(&s_lock);
        s_busy = -1;
        portEXIT_CRITICAL(&s_lock);
        const int64_t now = esp_timer_get_time();
        if (now - s_last_log >= 2000000) {
            ESP_LOGI(TAG, "shown=%lu replaced=%lu per 2 s, ppa avg=%lld us", (unsigned long)s_shown,
                     (unsigned long)s_replaced, (long long)(s_shown ? s_ppa_us / s_shown : 0));
            s_shown = s_replaced = 0;
            s_ppa_us = 0;
            s_last_log = now;
        }
    }
}

static uint16_t* alloc_rgb(void) {
    // Reserve internal RAM for VRAM, the indexed renderer, USB endpoints and
    // I2S DMA. Two 150 KiB RGB staging pictures exhausted it once input/audio
    // were enabled. PPA reads aligned PSRAM directly.
    return heap_caps_aligned_calloc(128, 1, RGB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
}

bool md_video_init(md_board_t* board) {
    s_board = board;
    s_vsync = xSemaphoreCreateBinary();
    esp_lcd_dpi_panel_event_callbacks_t cbs = {.on_refresh_done = on_refresh_done};
    esp_lcd_dpi_panel_register_event_callbacks(board->panel, &cbs, NULL);
    s_cur = 0;
    md_video_clear(0);
    ppa_client_config_t ppa_cfg = {.oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1};
    s_rgb[0] = alloc_rgb();
    s_rgb[1] = alloc_rgb();
    if (!s_rgb[0] || !s_rgb[1] || ppa_register_client(&ppa_cfg, &s_ppa) != ESP_OK) {
        ESP_LOGE(TAG, "PPA or picture buffers unavailable");
        return false;
    }
    s_last_log = esp_timer_get_time();
    return xTaskCreatePinnedToCore(presenter_task, "md_present", 4096, NULL, configMAX_PRIORITIES - 2,
                                   &s_presenter, 1) == pdPASS;
}

bool md_video_present(const uint8_t* lines, int width, int height, const uint16_t* palette) {
    portENTER_CRITICAL(&s_lock);
    // Never the picture the presenter reads; reuse an unclaimed one (newest frame wins).
    const int i = s_busy == 0 ? 1 : s_busy == 1 ? 0 : (s_ready >= 0 ? s_ready : 0);
    const bool replacing = s_ready == i;
    s_ready = -1;
    portEXIT_CRITICAL(&s_lock);
    if (replacing)
        ++s_replaced;

    uint16_t* out = s_rgb[i];
    for (int y = 0; y < height; ++y) {
        const uint8_t* src = lines + y * 320;
        for (int x = 0; x < width; ++x)
            *out++ = palette[src[x]];
    }
    esp_cache_msync(s_rgb[i], RGB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    s_rgb_w[i] = width;
    s_rgb_h[i] = height;

    portENTER_CRITICAL(&s_lock);
    s_ready = i;
    portEXIT_CRITICAL(&s_lock);
    xTaskNotifyGive(s_presenter);
    return true;
}
