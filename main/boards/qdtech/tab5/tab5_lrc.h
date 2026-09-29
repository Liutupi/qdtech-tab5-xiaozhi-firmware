#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tab5_lrc {

struct Line {
    uint32_t at_ms = 0;
    std::string text;
};

struct Document {
    std::vector<Line> lines;
    bool timed = false;
};

// Bounded parser for standard [mm:ss.xx] or [mm:ss.xxx] LRC. Plain lines are
// retained as an unsynchronized fallback. No network or LVGL dependencies.
bool Parse(const std::string& input, Document& output);
int ActiveLine(const Document& document, uint32_t elapsed_ms);

}  // namespace tab5_lrc
