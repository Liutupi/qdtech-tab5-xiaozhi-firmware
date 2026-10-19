#include "radio_catalog.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include "nabo_radio_catalog.h"
#include "tab5_sd.h"

namespace {
constexpr const char* TAG = "RadioCatalog";
constexpr int kNameSlots = 16;
constexpr size_t kNameBytes = 96;
constexpr size_t kFindBufferBytes = 4096;
constexpr const char* kListTypes[] = {"综合", "新闻", "音乐", "交通", "文艺", "财经"};

void* PsramAlloc(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void PsramFree(void* p) { heap_caps_free(p); }

void LowerAscii(std::string* text) {
    for (auto& c : *text)
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
}

// QingTing serves each live stream from three hosts; try the others when one fails.
void AddQingTingFallbacks(RadioStationInfo* info) {
    const std::string& url = info->urls[0];
    const size_t at = url.find("://lhttp.qtfm.cn/");
    if (at == std::string::npos)
        return;
    const std::string path = url.substr(at + std::strlen("://lhttp.qtfm.cn"));
    info->urls[1] = "https://lhttp-hw.qtfm.cn" + path;
    info->urls[2] = "http://lhttp.qingting.fm" + path;
}
}  // namespace

struct RadioCatalog::NameSlot {
    int index;
    char name[kNameBytes];
};

struct RadioCatalog::Card {
    Card() : catalog(PsramAlloc, PsramFree) {}
    nabo_radio_catalog::Catalog catalog;
    std::mutex mutex;
    // One PSRAM block: string scratch, then the name cache.
    char* scratch = nullptr;
    NameSlot* names = nullptr;
    int next_slot = 0;
};

uint16_t RadioCatalog::ListTypeBit(const char* type) {
    for (size_t i = 0; i < sizeof(kListTypes) / sizeof(kListTypes[0]); ++i)
        if (type && std::strcmp(type, kListTypes[i]) == 0)
            return uint16_t(1u << i);
    return 0;
}

RadioCatalog& RadioCatalog::Instance() {
    static RadioCatalog catalog;
    return catalog;
}

void RadioCatalog::EnsureLoaded(const std::function<std::vector<RadioListEntry>()>& fallback,
                                const char* card_path) {
    std::call_once(loaded_, [this, &fallback, card_path] {
        const size_t free_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (Tab5SdReady()) {
            auto* card = new (std::nothrow) Card();
            const size_t scratch_bytes = nabo_radio_catalog::kMaxStringsBytes;
            void* block = PsramAlloc(scratch_bytes + kNameSlots * sizeof(NameSlot));
            if (card && block && card->catalog.Open(card_path)) {
                card->scratch = static_cast<char*>(block);
                card->names = reinterpret_cast<NameSlot*>(card->scratch + scratch_bytes);
                for (int i = 0; i < kNameSlots; ++i)
                    card->names[i].index = -1;
                card_ = card;
                ESP_LOGI(TAG, "card catalog: %d stations, %d regions, index %u B, psram used %u B",
                         card->catalog.Count(), card->catalog.RegionCount(),
                         unsigned(card->catalog.ResidentBytes()),
                         unsigned(free_before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
            } else {
                ESP_LOGI(TAG, "no usable %s; using the small station list", card_path);
                PsramFree(block);
                delete card;
            }
        }
        if (!card_)
            list_ = fallback ? fallback() : std::vector<RadioListEntry>{};
        music_mask_ = TypeMask("音乐");
        news_mask_ = TypeMask("新闻");
        traffic_mask_ = TypeMask("交通");
        arts_mask_ = TypeMask("文艺");
    });
}

int RadioCatalog::Count() const { return card_ ? card_->catalog.Count() : int(list_.size()); }

size_t RadioCatalog::ResidentBytes() const {
    if (!card_)
        return 0;
    return card_->catalog.ResidentBytes() + nabo_radio_catalog::kMaxStringsBytes +
           kNameSlots * sizeof(NameSlot);
}

uint32_t RadioCatalog::StableId(int index) const {
    if (index < 0 || index >= Count())
        return 0;
    if (card_)
        return card_->catalog.At(index).id;
    return nabo_radio_catalog::StableIdForUrl(list_[index].info.urls[0].c_str());
}

int RadioCatalog::IndexOfId(uint32_t id) const {
    if (!id)
        return -1;
    const int count = Count();
    for (int i = 0; i < count; ++i)
        if (StableId(i) == id)
            return i;
    return -1;
}

int RadioCatalog::RegionId(int index) const {
    if (index < 0 || index >= Count())
        return -1;
    if (card_)
        return card_->catalog.At(index).region;
    // Small lists: the first station of the same region stands for it.
    for (int i = 0; i <= index; ++i)
        if (std::strcmp(list_[i].region, list_[index].region) == 0)
            return i;
    return index;
}

const char* RadioCatalog::RegionName(int index) const {
    if (index < 0 || index >= Count())
        return "";
    if (card_)
        return card_->catalog.RegionName(card_->catalog.At(index).region);
    return list_[index].region;
}

uint16_t RadioCatalog::Types(int index) const {
    if (index < 0 || index >= Count())
        return 0;
    return card_ ? card_->catalog.At(index).types : list_[index].types;
}

uint16_t RadioCatalog::TypeMask(const char* type) const {
    if (!card_)
        return ListTypeBit(type);
    for (int i = 0; i < card_->catalog.TypeCount(); ++i)
        if (std::strcmp(card_->catalog.TypeName(i), type) == 0)
            return uint16_t(1u << i);
    return 0;
}

const char* RadioCatalog::TypeName(int bit) const {
    if (card_)
        return card_->catalog.TypeName(bit);
    return bit >= 0 && bit < int(sizeof(kListTypes) / sizeof(kListTypes[0])) ? kListTypes[bit] : "";
}

const char* RadioCatalog::HomeBand(int index) const {
    const uint16_t types = Types(index);
    if (types & music_mask_)
        return "音乐";
    if (types & news_mask_)
        return "新闻";
    if (types & traffic_mask_)
        return "交通";
    if (types & arts_mask_)
        return "文艺";
    switch (nabo_radio_catalog::ClassifyRegion(RegionName(index))) {
        case nabo_radio_catalog::RegionClass::Overseas:
            return "港澳·海外";
        case nabo_radio_catalog::RegionClass::Province:
            return "各省";
        default:
            return "";
    }
}

bool RadioCatalog::CopyName(int index, char* out, size_t size) {
    if (!size || index < 0 || index >= Count())
        return false;
    if (!card_) {
        std::strncpy(out, list_[index].info.name.c_str(), size - 1);
        out[size - 1] = 0;
        return true;
    }
    std::lock_guard<std::mutex> lock(card_->mutex);
    for (int i = 0; i < kNameSlots; ++i) {
        if (card_->names[i].index == index) {
            std::strncpy(out, card_->names[i].name, size - 1);
            out[size - 1] = 0;
            return true;
        }
    }
    nabo_radio_catalog::Strings s;
    const bool ok =
        card_->catalog.ReadStrings(index, card_->scratch, nabo_radio_catalog::kMaxStringsBytes, &s);
    card_->catalog.CloseFile();
    if (!ok) {
        ESP_LOGW(TAG, "station %d unreadable on the card", index);
        return false;
    }
    NameSlot& slot = card_->names[card_->next_slot];
    card_->next_slot = (card_->next_slot + 1) % kNameSlots;
    slot.index = index;
    std::strncpy(slot.name, s.name, kNameBytes - 1);
    slot.name[kNameBytes - 1] = 0;
    std::strncpy(out, slot.name, size - 1);
    out[size - 1] = 0;
    return true;
}

std::string RadioCatalog::Name(int index) {
    char name[kNameBytes];
    return CopyName(index, name, sizeof(name)) ? std::string(name) : std::string();
}

std::string RadioCatalog::Label(int index) {
    std::string label = RegionName(index);
    const uint16_t types = Types(index);
    for (int bit = 0; bit < 16; ++bit) {
        if (!(types & (1u << bit)))
            continue;
        if (!label.empty())
            label += " · ";
        label += TypeName(bit);
        break;
    }
    return label;
}

bool RadioCatalog::Load(int index, RadioStationInfo* out) {
    if (index < 0 || index >= Count())
        return false;
    if (!card_) {
        *out = list_[index].info;
        return true;
    }
    std::lock_guard<std::mutex> lock(card_->mutex);
    nabo_radio_catalog::Strings s;
    const bool ok =
        card_->catalog.ReadStrings(index, card_->scratch, nabo_radio_catalog::kMaxStringsBytes, &s);
    card_->catalog.CloseFile();
    if (!ok) {
        ESP_LOGW(TAG, "station %d unreadable on the card", index);
        return false;
    }
    out->name = s.name;
    out->urls[0] = s.url;
    out->urls[1].clear();
    out->urls[2].clear();
    out->codec = "MP3";
    out->bitrate_kbps = std::strstr(s.url, "64k") ? 64 : 128;
    AddQingTingFallbacks(out);
    return true;
}

int RadioCatalog::Find(const std::string& needle, std::string* name) {
    std::string lowered = needle;
    LowerAscii(&lowered);
    if (lowered.empty())
        return -1;
    int found = -1;
    if (card_) {
        char* buffer = static_cast<char*>(PsramAlloc(kFindBufferBytes));
        if (!buffer)
            return -1;
        {
            std::lock_guard<std::mutex> lock(card_->mutex);
            found = card_->catalog.Find(lowered.c_str(), buffer, kFindBufferBytes);
            card_->catalog.CloseFile();
        }
        PsramFree(buffer);
    } else {
        int best_score = 0;
        for (int i = 0; i < int(list_.size()) && best_score < 4; ++i) {
            const int score =
                nabo_radio_catalog::MatchScore(list_[i].info.name.c_str(), "", lowered.c_str());
            if (score > best_score) {
                best_score = score;
                found = i;
            }
        }
    }
    if (found >= 0 && name)
        *name = Name(found);
    return found;
}

std::vector<RadioBandStations> RadioCatalog::Bands(const std::vector<uint32_t>& recent,
                                                   const std::string& home_region) const {
    using nabo_radio_catalog::ClassifyRegion;
    using nabo_radio_catalog::RegionClass;
    const int count = Count();
    std::vector<RadioBandStations> bands;
    RadioBandStations recent_band{"最近", {}};
    for (uint32_t id : recent) {
        const int index = IndexOfId(id);
        if (index >= 0)
            recent_band.stations.push_back(uint16_t(index));
    }
    bands.push_back(std::move(recent_band));

    auto add = [&](const char* name, auto&& keep) {
        RadioBandStations band{name, {}};
        for (int i = 0; i < count; ++i)
            if (keep(i))
                band.stations.push_back(uint16_t(i));
        if (!band.stations.empty()) {
            band.stations.shrink_to_fit();
            bands.push_back(std::move(band));
        }
    };
    add("本地", [&](int i) { return !home_region.empty() && home_region == RegionName(i); });
    add("新闻", [&](int i) { return (Types(i) & news_mask_) != 0; });
    add("音乐", [&](int i) { return (Types(i) & music_mask_) != 0; });
    add("交通", [&](int i) { return (Types(i) & traffic_mask_) != 0; });
    add("文艺", [&](int i) { return (Types(i) & arts_mask_) != 0; });

    // 各省 runs province by province (biggest first on the card), hottest
    // first within each.
    RadioBandStations provinces{"各省", {}};
    for (int i = 0; i < count; ++i)
        if (ClassifyRegion(RegionName(i)) == RegionClass::Province)
            provinces.stations.push_back(uint16_t(i));
    std::stable_sort(provinces.stations.begin(), provinces.stations.end(),
                     [this](uint16_t a, uint16_t b) { return RegionId(a) < RegionId(b); });
    if (!provinces.stations.empty())
        bands.push_back(std::move(provinces));

    add("港澳·海外", [&](int i) { return ClassifyRegion(RegionName(i)) == RegionClass::Overseas; });
    return bands;
}
