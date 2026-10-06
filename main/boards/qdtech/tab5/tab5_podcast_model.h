#pragma once

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

// Muse daily music-radio episodes as published by the NAS relay (/podcast/<token>).
// Header-only and free of ESP/LVGL so it is host-tested.
namespace tab5_podcast {

// Episode text is many small strings; under CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL they
// would all land in scarce internal RAM. Keep every byte of it in PSRAM.
template <typename T>
struct PsramAllocator {
    using value_type = T;
    PsramAllocator() noexcept = default;
    template <typename U>
    PsramAllocator(const PsramAllocator<U>&) noexcept {}
    T* allocate(std::size_t n) {
#ifdef ESP_PLATFORM
        void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        void* p = std::malloc(n * sizeof(T));
#endif
        if (!p)
            std::abort();  // built with -fno-exceptions
        return static_cast<T*>(p);
    }
    void deallocate(T* p, std::size_t) noexcept {
#ifdef ESP_PLATFORM
        heap_caps_free(p);
#else
        std::free(p);
#endif
    }
    template <typename U>
    bool operator==(const PsramAllocator<U>&) const noexcept { return true; }
    template <typename U>
    bool operator!=(const PsramAllocator<U>&) const noexcept { return false; }
};

using Str = std::basic_string<char, std::char_traits<char>, PsramAllocator<char>>;
inline Str ToStr(std::string_view v) { return Str(v.data(), v.size()); }

struct Track {
    Str title;
    Str artist;
};
using TrackList = std::vector<Track, PsramAllocator<Track>>;

struct Episode {
    int id = 0;
    Str title;
    Str script;
    Str from;
    Str time;
    Str audio_url;
    TrackList tracks;
};
using EpisodeList = std::vector<Episode, PsramAllocator<Episode>>;
// Immutable once published: snapshots and pages share it instead of copying text.
using SharedEpisodes = std::shared_ptr<const EpisodeList>;
inline std::shared_ptr<EpisodeList> MakeEpisodes() {
    return std::allocate_shared<EpisodeList>(PsramAllocator<EpisodeList>());
}

constexpr size_t kMaxTracks = 30;

inline std::string_view TrimView(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
        --b;
    return s.substr(a, b - a);
}
inline std::string Trim(std::string_view s) { return std::string(TrimView(s)); }

// Parses "1.《歌名》— 歌手" style lines (same rules as the relay's parseTracks), used when
// an older relay only provides the plain-text daily radio post.
inline TrackList ParseTracks(std::string_view text) {
    static constexpr std::string_view kOpen = "《", kClose = "》";
    static constexpr std::string_view kDashes[] = {"——", "—", "–", "－", "-", "：", ":"};
    TrackList tracks;
    size_t start = 0;
    while (start <= text.size() && tracks.size() < kMaxTracks) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        const std::string_view line = TrimView(text.substr(start, end - start));
        start = end + 1;
        size_t i = 0;
        while (i < line.size() && i < 2 && line[i] >= '0' && line[i] <= '9')
            ++i;
        if (i == 0)
            continue;
        const size_t open = line.find(kOpen, i);
        if (open == std::string_view::npos)
            continue;
        // Only a list marker ("." "、" ")" "）" and spaces) may sit between number and title.
        const std::string_view marker = TrimView(line.substr(i, open - i));
        if (!marker.empty() && marker != "." && marker != "、" && marker != ")" && marker != "）")
            continue;
        const size_t close = line.find(kClose, open + kOpen.size());
        if (close == std::string_view::npos || close == open + kOpen.size())
            continue;
        Track track;
        track.title = ToStr(TrimView(line.substr(open + kOpen.size(), close - open - kOpen.size())));
        std::string_view rest = TrimView(line.substr(close + kClose.size()));
        for (const std::string_view dash : kDashes) {
            while (rest.substr(0, dash.size()) == dash)
                rest = TrimView(rest.substr(dash.size()));
        }
        track.artist = ToStr(rest);
        tracks.push_back(std::move(track));
    }
    return tracks;
}

// Drops emoji and pictographs the device fonts cannot draw (U+2600-27BF, U+1F000+,
// variation selectors, ZWJ) and collapses the spaces they leave behind.
inline std::string DisplayText(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + n > s.size())
            break;
        unsigned cp = n == 1 ? c : c & (0x7f >> n);
        for (size_t k = 1; k < n; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3f);
        const bool drop = (cp >= 0x2600 && cp <= 0x27bf) || cp >= 0x1f000 || cp == 0xfe0f ||
                          cp == 0x200d;
        if (!drop && !(cp == ' ' && (out.empty() || out.back() == ' ' || out.back() == '\n')))
            out.append(s.data() + i, n);
        i += n;
    }
    return Trim(out);
}

// A daily-radio post in the plain inbox (relay without podcast support).
inline bool IsRadioPost(std::string_view from, std::string_view title) {
    return from == "每日电台" || title.find("音乐电台") != std::string_view::npos ||
           title.find("音乐播客") != std::string_view::npos ||
           title.find("音乐博客") != std::string_view::npos;
}

// The leading lines of a script before its numbered song list: the episode's "文案".
inline std::string Intro(std::string_view script, size_t max_bytes = 240) {
    std::string out;
    size_t start = 0;
    while (start < script.size()) {
        size_t end = script.find('\n', start);
        if (end == std::string_view::npos)
            end = script.size();
        const std::string_view line = TrimView(script.substr(start, end - start));
        start = end + 1;
        if (line.empty())
            continue;
        if (line[0] >= '0' && line[0] <= '9')
            break;
        if (!out.empty())
            out += ' ';
        out += line;
        if (out.size() >= max_bytes)
            break;
    }
    return out;
}

}  // namespace tab5_podcast
