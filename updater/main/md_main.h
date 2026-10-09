#pragma once
// MD (Mega Drive) mode of the ota_0 app. The main firmware writes
// /sdcard/tab5/md/launch.txt ("rom=/sdcard/roms/md/xx.bin") and reboots into ota_0.
#include <stdbool.h>

#include "esp_partition.h"

// Runs a game when a launch request is on the SD card; returns immediately otherwise.
// The request file is deleted and the boot partition reset to `factory` before the game
// starts, so any reset or crash lands back in the main firmware.
void md_maybe_run(const esp_partition_t* factory);
