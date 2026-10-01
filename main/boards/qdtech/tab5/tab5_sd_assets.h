#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Large, optional resources that live on the SD card instead of flash
// (flash assets partition is full). Each file is downloaded once, verified with
// SHA-256, and kept under /sdcard/tab5/.
namespace tab5_sd_assets {

struct Asset {
    const char* name;      // file name under /sdcard/tab5/
    size_t size;           // exact byte size
    const char* sha256;    // lowercase hex
    std::vector<std::string> urls;  // tried in order
};

// The Noto "common" 30 px CJK font (≈7000 characters) used for chat text, song titles and
// lyrics. Without it the compiled "basic" font is used and rare characters show as boxes.
const Asset& TextFont();

// Full path on the SD card.
std::string PathOf(const Asset& asset);

// True when the file exists with the expected size.
bool Present(const Asset& asset);

// Download (blocking, needs Wi-Fi and a mounted SD card). Returns true when the verified
// file is in place. Safe to call again; it does nothing if the file is already present.
bool Ensure(const Asset& asset);

// Reads a present file into PSRAM. Caller owns the buffer (heap_caps_free); nullptr on failure.
void* LoadToPsram(const Asset& asset, size_t* size_out);

}  // namespace tab5_sd_assets
