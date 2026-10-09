#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "md_board.h"
bool md_audio_init(md_board_t* board);
void md_audio_submit(const int16_t* fm, size_t fm_count, const int16_t* psg, size_t psg_count,
                     int fps);
void md_audio_mute(void);
