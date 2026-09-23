#pragma once

/**
 * Mount Tab5 microSD at /sdcard and register LVGL FS letter 'S'.
 * Hardware: SDMMC 4-bit slot0 (CLK43/CMD44/D0-3=39-42) with LDO_VO4 (chan 4)
 * as SDMMC IO / card power — same as M5Tab5-UserDemo bsp_sdcard_init().
 */
bool Tab5SdMountAndRegisterFs();
bool Tab5SdReady();
