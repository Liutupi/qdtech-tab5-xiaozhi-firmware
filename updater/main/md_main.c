#include "md_main.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// Gwenesis defines its own BIT(); ESP-IDF headers above already did.
#undef BIT
#include "gwenesis.h"
#include "md_board.h"
#include "md_video.h"

static const char* TAG = "md";

#define LAUNCH_FILE "/sdcard/tab5/md/launch.txt"
#define MAX_ROM_BYTES (6 * 1024 * 1024)
#define AUDIO_SAMPLE_RATE 53267
#define AUDIO_BUFFER_LENGTH (AUDIO_SAMPLE_RATE / 60 + 1)
#define STAGE1_RUN_SECONDS 90  // no gamepad yet: return to the main firmware on its own

// ---- Symbols the Gwenesis core expects from the host (as in retro-go's gwenesis app).
extern unsigned char* VRAM;
extern int zclk;
int system_clock;
int scan_line;
int16_t gwenesis_sn76489_buffer[AUDIO_BUFFER_LENGTH];
int sn76489_index;
int sn76489_clock;
int16_t gwenesis_ym2612_buffer[AUDIO_BUFFER_LENGTH];
int ym2612_index;
int ym2612_clock;

SaveState* saveGwenesisStateOpenForRead(const char* fileName) { return NULL; }
SaveState* saveGwenesisStateOpenForWrite(const char* fileName) { return NULL; }
int saveGwenesisStateGet(SaveState* state, const char* tagName) { return 0; }
void saveGwenesisStateSet(SaveState* state, const char* tagName, int value) {}
void saveGwenesisStateGetBuffer(SaveState* state, const char* tagName, void* buffer, int length) {}
void saveGwenesisStateSetBuffer(SaveState* state, const char* tagName, void* buffer, int length) {}
void gwenesis_io_get_buttons(void) {}

typedef struct {
    char rom[256];
} launch_t;

static bool read_launch(launch_t* out) {
    FILE* f = fopen(LAUNCH_FILE, "r");
    if (!f)
        return false;
    memset(out, 0, sizeof(*out));
    char line[300];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "rom=", 4) == 0)
            strlcpy(out->rom, line + 4, sizeof(out->rom));
    }
    fclose(f);
    return out->rom[0] != 0;
}

// ROM into PSRAM, size rounded up to 64 KB (the core addresses whole banks).
static uint8_t* load_rom(const char* path, size_t* size_out) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0x200 || st.st_size > MAX_ROM_BYTES) {
        ESP_LOGE(TAG, "rom missing or too large: %s", path);
        return NULL;
    }
    const size_t alloc = ((size_t)st.st_size + 0xFFFF) & ~(size_t)0xFFFF;
    uint8_t* rom = heap_caps_calloc(1, alloc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    FILE* f = rom ? fopen(path, "rb") : NULL;
    if (!f) {
        free(rom);
        return NULL;
    }
    const size_t got = fread(rom, 1, st.st_size, f);
    fclose(f);
    if (got != (size_t)st.st_size) {
        free(rom);
        return NULL;
    }
    *size_out = st.st_size;
    return rom;
}

static void return_to_main(void) {
    ESP_LOGI(TAG, "back to the main firmware");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

static void run_game(uint8_t* rom, size_t rom_size) {
    extern unsigned char gwenesis_vdp_regs[0x20];
    extern unsigned int gwenesis_vdp_status;
    extern unsigned short CRAM565[256];
    extern unsigned int screen_width, screen_height;
    extern int hint_pending;

    VRAM = heap_caps_malloc(VRAM_MAX_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    // 320 x 240 indexed lines plus the renderer's overdraw space on both sides.
    uint8_t* frame_mem = heap_caps_calloc(1, 320 * 242 + 320, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!VRAM || !frame_mem) {
        ESP_LOGE(TAG, "no memory for VRAM/frame");
        return;
    }
    uint8_t* frame = frame_mem + 160;

    load_cartridge(rom, rom_size);
    power_on();
    reset_emulation();
    ESP_LOGI(TAG, "running, internal free %u, psram free %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    const int64_t frame_us = 1000000 / 60;
    int64_t next = esp_timer_get_time();
    const int64_t stop_at = next + (int64_t)STAGE1_RUN_SECONDS * 1000000;
    int skip = 0;
    uint32_t frames = 0, drawn = 0;
    int64_t emu_us = 0, last_log = next;

    while (esp_timer_get_time() < stop_at) {
        const int64_t t0 = esp_timer_get_time();
        const bool draw = skip == 0;
        const int lines_per_frame = REG1_PAL ? LINES_PER_FRAME_PAL : LINES_PER_FRAME_NTSC;
        int hint_counter = gwenesis_vdp_regs[10];
        screen_width = REG12_MODE_H40 ? 320 : 256;
        screen_height = REG1_PAL ? 240 : 224;
        gwenesis_vdp_set_buffer((unsigned short*)frame);
        gwenesis_vdp_render_config();

        system_clock = 0;
        zclk = 0;
        ym2612_clock = 0;
        ym2612_index = 0;
        sn76489_clock = 0;
        sn76489_index = 0;
        scan_line = 0;

        while (scan_line < lines_per_frame) {
            m68k_run(system_clock + VDP_CYCLES_PER_LINE);
            z80_run(system_clock + VDP_CYCLES_PER_LINE);
            if (draw && scan_line < (int)screen_height)
                gwenesis_vdp_render_line(scan_line);
            if (scan_line == 0 || scan_line > (int)screen_height)
                hint_counter = REG10_LINE_COUNTER;
            if (--hint_counter < 0) {
                if (REG0_LINE_INTERRUPT != 0 && scan_line <= (int)screen_height) {
                    hint_pending = 1;
                    if ((gwenesis_vdp_status & STATUS_VIRQPENDING) == 0)
                        m68k_update_irq(4);
                }
                hint_counter = REG10_LINE_COUNTER;
            }
            scan_line++;
            if (scan_line == (int)screen_height) {
                if (REG1_VBLANK_INTERRUPT != 0) {
                    gwenesis_vdp_status |= STATUS_VIRQPENDING;
                    m68k_set_irq(6);
                }
                z80_irq_line(1);
            }
            if (scan_line == (int)screen_height + 1)
                z80_irq_line(0);
            system_clock += VDP_CYCLES_PER_LINE;
        }
        gwenesis_SN76489_run(system_clock);
        ym2612_run(system_clock);
        m68k.cycles -= system_clock;

        const int64_t t1 = esp_timer_get_time();
        emu_us += t1 - t0;
        ++frames;
        if (draw) {
            md_video_present(frame, screen_width, screen_height, CRAM565);
            ++drawn;
        }

        // Pace to 60 Hz; skip drawing the next frame when we are behind.
        next += frame_us;
        const int64_t now = esp_timer_get_time();
        if (now < next) {
            skip = 0;
            vTaskDelay(pdMS_TO_TICKS((next - now) / 1000));
        } else {
            skip = (skip == 0 && now - next > frame_us / 2) ? 1 : 0;
            if (now - next > 10 * frame_us)
                next = now;  // way behind (e.g. loading): do not try to catch up
        }
        if (now - last_log >= 2000000) {
            ESP_LOGI(TAG, "emulated %lu frames, drawn %lu per 2 s, emu avg %lld us/frame",
                     (unsigned long)frames, (unsigned long)drawn, (long long)(emu_us / (frames ? frames : 1)));
            frames = drawn = 0;
            emu_us = 0;
            last_log = now;
        }
    }
}

void md_maybe_run(const esp_partition_t* factory) {
    launch_t launch;
    if (!read_launch(&launch))
        return;
    // One-shot: whatever happens next, the following boot is the main firmware.
    remove(LAUNCH_FILE);
    if (esp_ota_set_boot_partition(factory) != ESP_OK)
        ESP_LOGW(TAG, "could not reset the boot partition to factory");
    ESP_LOGI(TAG, "launch %s", launch.rom);

    size_t rom_size = 0;
    uint8_t* rom = load_rom(launch.rom, &rom_size);
    if (!rom)
        return_to_main();
    ESP_LOGI(TAG, "rom loaded: %u bytes", (unsigned)rom_size);

    static md_board_t board;
    if (!md_board_init(&board))
        return_to_main();
    md_video_init(&board);
    md_board_backlight(80);
    run_game(rom, rom_size);
    return_to_main();
}
