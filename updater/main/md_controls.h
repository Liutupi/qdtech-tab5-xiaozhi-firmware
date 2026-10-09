#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool md_controls_init(void);
uint8_t md_controls_buttons(void);
bool md_controls_exit(void);
#ifdef __cplusplus
}
#endif
