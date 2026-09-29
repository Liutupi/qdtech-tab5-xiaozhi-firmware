#include "tab5_lrc.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace tab5_lrc {
namespace {

constexpr size_t kMaxInputBytes = 16 * 1024;
constexpr size_t kMaxLines = 180;
constexpr size_t kMaxTextBytes = 160;

bool Digit(char ch) { return ch >= '0' && ch <= '9'; }

bool Timestamp(const std::string& tag, uint32_t& time_ms) {
    const size_t colon = tag.find(':');
    if (colon == std::string::npos || colon < 1 || colon > 3 || colon + 3 > tag.size())
        return false;
    unsigned minutes = 0;
    for (size_t i = 0; i < colon; ++i) {
        if (!Digit(tag[i]))
            return false;
        minutes = minutes * 10 + unsigned(tag[i] - '0');
    }
    if (!Digit(tag[colon + 1]) || !Digit(tag[colon + 2]))
        return false;
    const unsigned seconds = unsigned(tag[colon + 1] - '0') * 10 + unsigned(tag[colon + 2] - '0');
    if (seconds >= 60)
        return false;
    unsigned millis = 0;
    size_t decimals = 0;
    if (colon + 3 < tag.size()) {
        if (tag[colon + 3] != '.')
            return false;
        for (size_t i = colon + 4; i < tag.size(); ++i) {
            if (!Digit(tag[i]) || ++decimals > 3)
                return false;
            millis = millis * 10 + unsigned(tag[i] - '0');
        }
        if (decimals == 0)
            return false;
        while (decimals++ < 3)
            millis *= 10;
    }
    time_ms = (minutes * 60 + seconds) * 1000 + millis;
    return true;
}

std::string Trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

}  // namespace

bool Parse(const std::string& input, Document& output) {
    output = {};
    if (input.empty() || input.size() > kMaxInputBytes)
        return false;
    std::vector<std::string> plain_lines;
    int offset_ms = 0;
    size_t begin = 0;
    while (begin < input.size()) {
        const size_t end = input.find('\n', begin);
        const std::string raw =
            Trim(input.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        begin = end == std::string::npos ? input.size() : end + 1;
        if (raw.empty())
            continue;
        if (raw.rfind("[offset:", 0) == 0 && raw.back() == ']') {
            const std::string offset = raw.substr(8, raw.size() - 9);
            if (!offset.empty()) {
                size_t pos = offset[0] == '-' || offset[0] == '+' ? 1 : 0;
                bool valid = pos < offset.size();
                for (size_t i = pos; i < offset.size(); ++i)
                    valid &= Digit(offset[i]);
                if (valid) {
                    const int sign = offset[0] == '-' ? -1 : 1;
                    int value = 0;
                    for (size_t i = pos; i < offset.size() && value < 100000; ++i)
                        value = value * 10 + (offset[i] - '0');
                    offset_ms = sign * std::min(value, 100000);
                }
            }
            continue;
        }

        std::vector<uint32_t> stamps;
        size_t cursor = 0;
        while (cursor < raw.size() && raw[cursor] == '[') {
            const size_t close = raw.find(']', cursor);
            if (close == std::string::npos)
                break;
            uint32_t stamp = 0;
            if (!Timestamp(raw.substr(cursor + 1, close - cursor - 1), stamp))
                break;
            stamps.push_back(stamp);
            cursor = close + 1;
        }
        const std::string content = Trim(raw.substr(cursor));
        if (content.empty() || content.size() > kMaxTextBytes)
            continue;
        if (stamps.empty()) {
            // Ignore LRC metadata [ti:], [ar:], etc.
            if (raw.front() != '[' && plain_lines.size() < kMaxLines)
                plain_lines.push_back(content);
            continue;
        }
        for (const uint32_t stamp : stamps) {
            if (output.lines.size() >= kMaxLines)
                return false;
            output.lines.push_back({stamp, content});
        }
    }
    if (!output.lines.empty()) {
        output.timed = true;
        for (auto& line : output.lines) {
            const int64_t adjusted = int64_t(line.at_ms) + offset_ms;
            line.at_ms = static_cast<uint32_t>(std::max<int64_t>(0, adjusted));
        }
        std::stable_sort(output.lines.begin(), output.lines.end(),
                         [](const Line& a, const Line& b) { return a.at_ms < b.at_ms; });
    } else {
        for (auto& text : plain_lines)
            output.lines.push_back({0, std::move(text)});
    }
    return !output.lines.empty();
}

int ActiveLine(const Document& document, uint32_t elapsed_ms) {
    if (document.lines.empty())
        return -1;
    if (!document.timed)
        return 0;
    const auto it =
        std::upper_bound(document.lines.begin(), document.lines.end(), elapsed_ms,
                         [](uint32_t value, const Line& line) { return value < line.at_ms; });
    return it == document.lines.begin() ? -1 : int(it - document.lines.begin() - 1);
}

}  // namespace tab5_lrc
