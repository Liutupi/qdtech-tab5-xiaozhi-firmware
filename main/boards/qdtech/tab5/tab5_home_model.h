#pragma once

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "tab5_podcast_model.h"  // PsramAllocator / Str

// 米家中控 data: Home Assistant devices and combined scenes as published by the NAS relay
// (GET /home/<token>/devices?format=tsv). Header-only and free of ESP/LVGL so it is
// host-tested. Strings live in PSRAM: a catalog is dozens of short names.
namespace tab5_home {

using tab5_podcast::Str;
template <typename T>
using List = std::vector<T, tab5_podcast::PsramAllocator<T>>;

enum class Kind { kSwitch, kLight, kClimate, kMedia, kScene, kOther };

struct Device {
    Str id;      // HA entity id, e.g. switch.pzg_1127_2a2e_switch
    Str label;   // short display name, e.g. 卧室电脑开机
    Str domain;  // light, switch, climate, media_player, scene, ...
    Str state;   // on/off/cool/idle/unavailable/...
    Str area;    // room, may be empty
    int brightness = -1;      // 0-255, -1 unknown
    float target = -1000;     // climate target °C, -1000 unknown
    float current = -1000;    // climate room °C

    Kind kind() const {
        if (domain == "switch" || domain == "fan" || domain == "humidifier")
            return Kind::kSwitch;
        if (domain == "light")
            return Kind::kLight;
        if (domain == "climate")
            return Kind::kClimate;
        if (domain == "media_player")
            return Kind::kMedia;
        if (domain == "scene" || domain == "script")
            return Kind::kScene;
        return Kind::kOther;
    }
    bool available() const { return state != "unavailable" && state != "unknown"; }
    // HA scenes report a timestamp; climate "off" vs any mode; players off/standby.
    bool on() const {
        if (!available() || kind() == Kind::kScene)
            return false;
        return state != "off" && state != "standby" && state != "closed";
    }
};

struct Scene {
    Str id;
    Str name;
    Str room;
    bool has_on = false;
    bool has_off = false;
};

struct Catalog {
    List<Device> devices;
    List<Scene> scenes;
};

inline std::string_view Field(std::string_view line, int index) {
    size_t start = 0;
    for (int i = 0; i < index; ++i) {
        const size_t tab = line.find('\t', start);
        if (tab == std::string_view::npos)
            return {};
        start = tab + 1;
    }
    const size_t end = line.find('\t', start);
    return line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
}

inline float Number(std::string_view v, float fallback) {
    if (v.empty() || v == "-" || v.size() > 15)
        return fallback;
    char buffer[16];
    v.copy(buffer, v.size());
    buffer[v.size()] = '\0';
    char* end = nullptr;
    const float value = std::strtof(buffer, &end);
    return end && *end == '\0' ? value : fallback;
}

inline bool SameCatalog(const Catalog& a, const Catalog& b) {
    if (a.devices.size() != b.devices.size() || a.scenes.size() != b.scenes.size())
        return false;
    for (size_t i = 0; i < a.devices.size(); ++i) {
        const auto &x = a.devices[i], &y = b.devices[i];
        if (x.id != y.id || x.label != y.label || x.state != y.state || x.area != y.area ||
            x.brightness != y.brightness || x.target != y.target || x.current != y.current)
            return false;
    }
    for (size_t i = 0; i < a.scenes.size(); ++i) {
        const auto &x = a.scenes[i], &y = b.scenes[i];
        if (x.id != y.id || x.name != y.name || x.room != y.room || x.has_on != y.has_on ||
            x.has_off != y.has_off)
            return false;
    }
    return true;
}

constexpr size_t kMaxDevices = 96;
constexpr size_t kMaxScenes = 24;

// Returns false unless the payload starts with the "V\t1" version line.
inline bool ParseCatalog(std::string_view text, Catalog* out) {
    out->devices.clear();
    out->scenes.clear();
    bool versioned = false;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (!versioned) {
            if (line != "V\t1")
                return false;
            versioned = true;
            continue;
        }
        const std::string_view tag = Field(line, 0);
        if (tag == "D" && out->devices.size() < kMaxDevices) {
            Device d;
            d.id = tab5_podcast::ToStr(Field(line, 1));
            d.label = tab5_podcast::ToStr(Field(line, 2));
            d.domain = tab5_podcast::ToStr(Field(line, 3));
            d.state = tab5_podcast::ToStr(Field(line, 4));
            d.area = tab5_podcast::ToStr(Field(line, 5));
            d.brightness = int(Number(Field(line, 6), -1));
            d.target = Number(Field(line, 7), -1000);
            d.current = Number(Field(line, 8), -1000);
            if (!d.id.empty() && d.id.find('.') != Str::npos)
                out->devices.push_back(std::move(d));
        } else if (tag == "S" && out->scenes.size() < kMaxScenes) {
            Scene s;
            s.id = tab5_podcast::ToStr(Field(line, 1));
            s.name = tab5_podcast::ToStr(Field(line, 2));
            s.room = tab5_podcast::ToStr(Field(line, 3));
            s.has_on = Field(line, 4) == "1";
            s.has_off = Field(line, 5) == "1";
            if (!s.id.empty() && !s.name.empty())
                out->scenes.push_back(std::move(s));
        }
    }
    return versioned;
}

// "K\t<key>\n" from POST /home/<token>/pair?format=tsv.
inline std::string ParsePairKey(std::string_view text) {
    if (text.substr(0, 2) != "K\t")
        return {};
    std::string_view key = text.substr(2);
    while (!key.empty() && (key.back() == '\n' || key.back() == '\r'))
        key.remove_suffix(1);
    if (key.size() < 24 || key.size() > 64)
        return {};
    for (char c : key)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
              c == '_'))
            return {};
    return std::string(key);
}

// https://<tunnel>/inbox/<token> (or .../mcp/<token>) -> https://<tunnel>/home/<token>.
inline std::string HomeBase(std::string_view inbox_url) {
    for (std::string_view marker : {std::string_view("/inbox/"), std::string_view("/mcp/")}) {
        const size_t at = inbox_url.find(marker);
        if (at == std::string_view::npos)
            continue;
        std::string_view token = inbox_url.substr(at + marker.size());
        const size_t cut = token.find_first_of("/?#");
        if (cut != std::string_view::npos)
            token = token.substr(0, cut);
        if (token.empty())
            return {};
        return std::string(inbox_url.substr(0, at)) + "/home/" + std::string(token);
    }
    return {};
}

// Voice targets: "家庭影院", "卧室电脑", "书房的灯"... The longest scene name or device
// label contained in the request wins; a room in the request breaks ties between devices.
inline bool Contains(std::string_view text, std::string_view part) {
    return !part.empty() && text.find(part) != std::string_view::npos;
}
inline int MatchScene(const Catalog& c, std::string_view text) {
    int best = -1;
    size_t best_len = 0;
    for (size_t i = 0; i < c.scenes.size(); ++i) {
        const auto& name = c.scenes[i].name;
        if (Contains(text, {name.data(), name.size()}) && name.size() > best_len) {
            best = int(i);
            best_len = name.size();
        }
    }
    return best;
}
inline int MatchDevice(const Catalog& c, std::string_view text) {
    int best = -1;
    size_t best_score = 0;
    for (size_t i = 0; i < c.devices.size(); ++i) {
        const auto& d = c.devices[i];
        if (d.kind() == Kind::kOther || !Contains(text, {d.label.data(), d.label.size()}))
            continue;
        size_t score = d.label.size() * 4;
        if (Contains(text, {d.area.data(), d.area.size()}))
            score += d.area.size() * 4 + 1;
        if (d.available())
            score += 1;
        if (score > best_score) {
            best = int(i);
            best_score = score;
        }
    }
    if (best >= 0)
        return best;
    // "书房的灯", "卧室空调": a room in the request plus a word for the kind of device.
    auto kind_word = [](const Device& d) -> std::string_view {
        switch (d.kind()) {
            case Kind::kLight:
                return "灯";
            case Kind::kClimate:
                return "空调";
            case Kind::kMedia:
                return Contains({d.label.data(), d.label.size()}, "电视") ? "电视" : "音箱";
            default:
                return {};
        }
    };
    for (size_t i = 0; i < c.devices.size(); ++i) {
        const auto& d = c.devices[i];
        if (!Contains(text, {d.area.data(), d.area.size()}) || !Contains(text, kind_word(d)))
            continue;
        const size_t score = d.area.size() * 2 + (d.available() ? 1 : 0);
        if (score > best_score) {
            best = int(i);
            best_score = score;
        }
    }
    return best;
}

}  // namespace tab5_home
