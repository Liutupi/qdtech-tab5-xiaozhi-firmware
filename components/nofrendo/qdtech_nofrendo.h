#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*qd_nofrendo_frame_cb_t)(const uint16_t* pixels, uint16_t width, uint16_t height,
                                      void* user);
typedef void (*qd_nofrendo_audio_cb_t)(const int16_t* samples, int sample_count, int sample_rate,
                                       void* user);

// Indexed fast path: lines[y][x] are palette indices, palette is native RGB565.
// Return non-zero if the frame was consumed; 0 falls back to the RGB callback.
typedef int (*qd_nofrendo_indexed_frame_cb_t)(const uint8_t* const* lines, const uint16_t* palette,
                                              uint16_t width, uint16_t height, void* user);

void qd_nofrendo_set_indexed_frame_callback(qd_nofrendo_indexed_frame_cb_t callback, void* user);
void qd_nofrendo_set_frame_callback(qd_nofrendo_frame_cb_t callback, void* user);
void qd_nofrendo_set_audio_callback(qd_nofrendo_audio_cb_t callback, void* user);
void qd_nofrendo_set_controller(uint8_t controller);
void qd_nofrendo_request_stop(void);
int qd_nofrendo_run(const char* rom_path);

#ifdef __cplusplus
}
#endif
