#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "tab5_podcast_model.h"  // PsramAllocator / Str
#include "tab5_rom_names.h"

// Mega Drive games on the SD card: /sdcard/roms/md/catalog.tsv
// ("filename<TAB>title<TAB>category", first line is the header), as written for the
// earlier ESP32 board. Header-only and free of ESP/LVGL so it is host-tested.
namespace tab5_md {

using tab5_podcast::Str;

struct Game {
    Str file;      // e.g. 01_索尼克一代.bin (relative to the MD directory)
    Str title;     // e.g. 索尼克一代
    Str category;  // e.g. 动作
};
using Catalog = std::vector<Game, tab5_podcast::PsramAllocator<Game>>;

constexpr size_t kMaxGames = 200;

// A plain file name (no directories) ending in .bin, .gen, .smd or .md.
inline bool IsRomFile(std::string_view name) {
    if (name.size() < 4 || name.find('/') != std::string_view::npos || name.find("..") != std::string_view::npos)
        return false;
    const size_t dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0)
        return false;
    std::string ext(name.substr(dot + 1));
    for (char& c : ext)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    return ext == "bin" || ext == "gen" || ext == "smd" || ext == "md";
}

inline Catalog ParseCatalog(std::string_view text) {
    Catalog games;
    size_t start = 0;
    bool first = true;
    while (start < text.size() && games.size() < kMaxGames) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (first) {
            first = false;
            if (line.size() >= 3 && line.substr(0, 3) == "\xEF\xBB\xBF")
                line.remove_prefix(3);
            if (line.substr(0, 8) == "filename")
                continue;  // header
        }
        const size_t tab1 = line.find('\t');
        const std::string_view file = line.substr(0, tab1);
        if (!IsRomFile(file))
            continue;
        std::string_view title, category;
        if (tab1 != std::string_view::npos) {
            const size_t tab2 = line.find('\t', tab1 + 1);
            title = line.substr(tab1 + 1, tab2 == std::string_view::npos ? std::string_view::npos : tab2 - tab1 - 1);
            if (tab2 != std::string_view::npos)
                category = line.substr(tab2 + 1);
        }
        Game g;
        g.file = tab5_podcast::ToStr(file);
        g.title = title.empty() ? tab5_podcast::ToStr(tab5_roms::TidyTitle(file)) : tab5_podcast::ToStr(title);
        g.category = tab5_podcast::ToStr(category);
        games.push_back(std::move(g));
    }
    return games;
}

}  // namespace tab5_md
