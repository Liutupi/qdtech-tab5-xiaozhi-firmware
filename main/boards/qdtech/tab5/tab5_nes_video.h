#pragma once

#include <cstdint>

#include <esp_lcd_types.h>

#include "lvgl.h"

// Direct NES video for the Tab5 MIPI-DPI panel.
//
// The old path scaled every NES frame to a 960x720 RGB565 image on the CPU,
// then LVGL re-rendered that image in 720x50 strips, software-rotated each
// strip and DMA2D-copied it into a single scan-out buffer: ~3 full-screen
// PSRAM passes per frame (16-22 fps) plus tearing.
//
// Here the emulator task writes palette-indexed NES lines straight into the
// back frame buffer (palette lookup + nearest-neighbour scale + 90 degree
// rotation in one pass), then flips the DPI panel to it. LVGL is paused while
// a game runs so nothing else draws into the panel, and flips are aligned to
// panel VSYNC so there is no tearing.
namespace tab5_nes_video {

enum Aspect : int {
    kAspectPixelPerfect = 0,  // 768x720, exact 3x3 pixels
    kAspect4x3 = 1,           // 960x720, CRT-like 4:3
};

// Call once after the LVGL display exists. Needs a DPI panel created with
// num_fbs >= 2; otherwise the caller keeps using the LVGL fallback.
bool Init(esp_lcd_panel_handle_t panel, lv_display_t* display);
bool Available();

// Emulator task, around one ROM session.
bool Begin();
void End();
bool Active();

// Emulator task, once per rendered NES frame. Returns true if the frame was
// consumed (also when intentionally dropped to stay VSYNC-aligned).
bool Present(const uint8_t* const* lines, const uint16_t* palette, uint16_t width,
             uint16_t height);

void SetAspect(int aspect);
int GetAspect();

}  // namespace tab5_nes_video
