#pragma once
// Mega Drive frames on the Tab5 panel: palette-indexed 320/256 x 224/240 lines are
// expanded to RGB565, then the PPA scales them 3x and rotates them 90 degrees into the
// portrait scan-out buffers, flipped on VSYNC by a presenter task on core 1.
#include <stdbool.h>
#include <stdint.h>

#include "md_board.h"

// Starts the presenter task (core 1). False when the PPA or buffers are unavailable.
bool md_video_init(md_board_t* board);
// lines: width x height bytes, stride 320. palette: 256 native RGB565 entries.
// Converts on the calling core and hands the picture to the presenter; never blocks.
bool md_video_present(const uint8_t* lines, int width, int height, const uint16_t* palette);
// Fill both buffers with one colour (e.g. black before showing the first frame).
void md_video_clear(uint16_t color);
