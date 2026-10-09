#pragma once
// Minimal Tab5 bring-up for the MD (Mega Drive) mode of the ota_0 app: I2C, the two
// PI4IOE5V6408 expanders (LCD/touch reset, speaker and USB 5 V power), the ST7121
// MIPI-DSI panel with two scan-out buffers, and the PWM backlight. Mirrors the main
// firmware's QdtechTab5Board (main/boards/qdtech/tab5/m5stack_tab5.cc).
#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_lcd_types.h"

#define MD_PANEL_W 720
#define MD_PANEL_H 1280

typedef struct {
    i2c_master_bus_handle_t i2c;
    esp_lcd_panel_handle_t panel;
    uint16_t* fb[2];  // portrait 720x1280 RGB565 scan-out buffers
} md_board_t;

// Returns false when the panel is not supported here (only ST7121 so far); the caller
// then goes straight back to the main firmware.
bool md_board_init(md_board_t* board);
void md_board_backlight(int percent);
