#pragma once
// Mega Drive frames on the Tab5 panel: palette-indexed 320/256 x 224/240 lines,
// 3x nearest-neighbour scale and the 90 degree rotation into the portrait scan-out
// buffers in one pass, flipped on VSYNC (same scheme as the main firmware's
// tab5_nes_video).
#include <stdbool.h>
#include <stdint.h>

#include "md_board.h"

void md_video_init(md_board_t* board);
// lines: width x height bytes, stride 320. palette: 256 native RGB565 entries.
// Returns false when the previous flip has not reached VSYNC yet (frame skipped).
bool md_video_present(const uint8_t* lines, int width, int height, const uint16_t* palette);
// Fill both buffers with one colour (e.g. black before showing the first frame).
void md_video_clear(uint16_t color);
