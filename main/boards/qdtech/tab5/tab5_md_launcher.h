#pragma once

#include <memory>
#include <string>

#include "tab5_md_catalog.h"

// Starts Mega Drive games, which run in the ota_0 app (updater 1.1+, Gwenesis) with the
// whole chip to itself: write /sdcard/tab5/md/launch.txt, select ota_0 and reboot. That app
// deletes the request and points the boot back at factory before the game starts.
namespace tab5_md {

constexpr const char* kRomDir = "/sdcard/roms/md";

// Reads kRomDir/catalog.tsv. Empty when the SD card or catalog is missing.
std::shared_ptr<const Catalog> LoadCatalog();

// Main task. Returns an error message for the user, or does not return (reboots).
std::string Launch(const Game& game);

}  // namespace tab5_md
