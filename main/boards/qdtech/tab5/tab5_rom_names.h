#pragma once

#include <string>
#include <string_view>

// Game list titles from ROM file names such as "106.冒险岛2无限人.nes",
// "07_战斧三代.bin" or "吞食天地2 [先锋卡通汉化 (laopix简体中文名字版)].nes":
// drop the extension, the leading catalogue number and trailing [credits], and turn
// underscores into spaces. Header-only; host-tested.
namespace tab5_roms {

inline std::string TidyTitle(std::string_view file_name) {
    std::string_view name = file_name;
    const size_t slash = name.find_last_of('/');
    if (slash != std::string_view::npos)
        name.remove_prefix(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string_view::npos && dot > 0 && name.size() - dot <= 4)
        name = name.substr(0, dot);

    // "106." / "07_" / "3-" / "12 " catalogue prefixes (1-3 digits).
    size_t digits = 0;
    while (digits < name.size() && digits < 4 && name[digits] >= '0' && name[digits] <= '9')
        ++digits;
    if (digits >= 1 && digits <= 3 && digits < name.size()) {
        const char sep = name[digits];
        if (sep == '.' || sep == '_' || sep == '-' || sep == ' ') {
            std::string_view rest = name.substr(digits + 1);
            while (!rest.empty() && rest.front() == ' ')
                rest.remove_prefix(1);
            if (!rest.empty())
                name = rest;
        }
    }

    std::string out(name);
    // Trailing "[translator / dumper]" groups.
    while (true) {
        size_t end = out.size();
        while (end > 0 && out[end - 1] == ' ')
            --end;
        if (end == 0 || out[end - 1] != ']')
            break;
        const size_t open = out.rfind('[', end - 1);
        if (open == std::string::npos || open == 0)
            break;
        out.erase(open);
    }
    for (char& c : out)
        if (c == '_')
            c = ' ';
    // Collapse doubled spaces and trim.
    std::string tidy;
    for (char c : out)
        if (!(c == ' ' && (tidy.empty() || tidy.back() == ' ')))
            tidy += c;
    while (!tidy.empty() && tidy.back() == ' ')
        tidy.pop_back();
    return tidy.empty() ? std::string(file_name) : tidy;
}

}  // namespace tab5_roms
