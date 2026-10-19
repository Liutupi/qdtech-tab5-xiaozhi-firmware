#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// A station's playback fields, copied out of the catalog for one use.
struct RadioStationInfo {
    std::string name;
    std::string urls[3];
    std::string codec = "MP3";
    int bitrate_kbps = 0;
};

// One entry of a small in-RAM list: the built-in table or /sdcard/radio.json.
struct RadioListEntry {
    RadioStationInfo info;
    const char* region = "";  // static string, e.g. "广东", "网络台", "海外"
    uint16_t types = 0;       // bits of RadioCatalog::ListTypeBit()
};

// One band on the NABO FM dial: catalog indices in dial order.
struct RadioBandStations {
    const char* name;
    std::vector<uint16_t> stations;
};

// Station directory: the NABO FM catalog on the SD card when present (only a
// small index stays in PSRAM; names and URLs are read from the card), else a
// small in-RAM list. Loaded once, then read-only; safe from any task.
class RadioCatalog {
public:
    static constexpr const char* kCardPath = "/sdcard/tab5/nabo/radio-v1/catalog.bin";
    // Program types a small-list entry can carry.
    static uint16_t ListTypeBit(const char* type);

    // The firmware's directory; host tests construct their own.
    static RadioCatalog& Instance();
    RadioCatalog() = default;
    RadioCatalog(const RadioCatalog&) = delete;
    RadioCatalog& operator=(const RadioCatalog&) = delete;

    // The first call loads the card catalog, or `fallback()` without one.
    void EnsureLoaded(const std::function<std::vector<RadioListEntry>()>& fallback,
                      const char* card_path = kCardPath);
    bool FromCard() const { return card_ != nullptr; }
    int Count() const;
    size_t ResidentBytes() const;

    uint32_t StableId(int index) const;
    int IndexOfId(uint32_t id) const;
    int RegionId(int index) const;
    const char* RegionName(int index) const;
    // 音乐, 新闻, 交通, 文艺, 港澳·海外, 各省, or "" for the rest.
    const char* HomeBand(int index) const;

    std::string Name(int index);
    // "广东 · 交通"
    std::string Label(int index);
    bool Load(int index, RadioStationInfo* out);
    // Best match by name or pinyin initials; -1 when none.
    int Find(const std::string& needle, std::string* name);

    // 最近, 本地, 新闻, 音乐, 交通, 文艺, 各省, 港澳·海外; empty bands other
    // than 最近 are left out. `recent` is newest first.
    std::vector<RadioBandStations> Bands(const std::vector<uint32_t>& recent,
                                         const std::string& home_region) const;

private:
    struct Card;
    struct NameSlot;

    uint16_t Types(int index) const;
    uint16_t TypeMask(const char* type) const;
    const char* TypeName(int bit) const;
    bool CopyName(int index, char* out, size_t size);

    std::once_flag loaded_;
    Card* card_ = nullptr;
    std::vector<RadioListEntry> list_;
    uint16_t music_mask_ = 0;
    uint16_t news_mask_ = 0;
    uint16_t traffic_mask_ = 0;
    uint16_t arts_mask_ = 0;
};
