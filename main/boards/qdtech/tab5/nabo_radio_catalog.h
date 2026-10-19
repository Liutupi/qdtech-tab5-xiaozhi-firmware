#pragma once

// NABO FM station catalog on the SD card (scripts/prepare_nabo_radio_catalog.py).
//
// Little-endian file, version 1:
//   header (40 bytes)
//   region table: u32 file offset of each NUL-terminated region name
//   type table:   u32 file offset of each NUL-terminated program-type name
//   index:        one 16-byte Record per station
//   names:        the region and type names
//   --- strings_offset ---
//   strings:      per station "name\0url\0py\0", in index order
//
// Everything before strings_offset (about 18 KB for 1,100 stations) is read
// once into a single buffer; a station's strings are read from the card when
// needed. index_crc covers bytes [header_size, strings_offset).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace nabo_radio_catalog {

constexpr char kMagic[4] = {'N', 'F', 'M', 'C'};
constexpr uint16_t kVersion = 1;
constexpr size_t kHeaderSize = 40;
constexpr size_t kRecordSize = 16;
constexpr size_t kMaxStations = 4000;
constexpr size_t kMaxTypes = 16;
constexpr size_t kMaxRegions = 255;
// name + url + py, each NUL-terminated, always fits this.
constexpr size_t kMaxStringsBytes = 3 * 256;

inline uint16_t ReadU16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t ReadU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline uint32_t Crc32(const uint8_t* data, size_t len, uint32_t crc = 0) {
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// Stable across catalog rebuilds and shared with the built-in list, so
// favorites survive both: the QingTing live id when the URL has one, else a
// hash of the URL with the top bit set.
inline uint32_t StableIdForUrl(const char* url) {
    if (!url || !*url)
        return 0;
    for (const char* host : {"qtfm.cn/live/", "qingting.fm/live/"}) {
        const char* at = std::strstr(url, host);
        if (!at)
            continue;
        at += std::strlen(host);
        uint32_t id = 0;
        int digits = 0;
        while (*at >= '0' && *at <= '9' && digits < 10) {
            id = id * 10 + uint32_t(*at++ - '0');
            ++digits;
        }
        if (digits && *at == '/' && id && id < 0x80000000u)
            return id;
    }
    return 0x80000000u |
           (Crc32(reinterpret_cast<const uint8_t*>(url), std::strlen(url)) & 0x7fffffffu);
}

struct Record {
    uint32_t id = 0;
    uint32_t strings = 0;  // offset from strings_offset
    uint16_t heat = 0;
    uint16_t types = 0;  // bit i = type table entry i
    uint8_t region = 0;
    uint8_t name_len = 0;
    uint8_t url_len = 0;
    uint8_t py_len = 0;
    size_t StringsBytes() const { return size_t(name_len) + url_len + py_len + 3; }
};

struct Strings {
    const char* name = "";
    const char* url = "";
    const char* py = "";
};

struct Header {
    uint16_t station_count = 0;
    uint16_t region_count = 0;
    uint16_t type_count = 0;
    uint32_t regions_offset = 0;
    uint32_t types_offset = 0;
    uint32_t index_offset = 0;
    uint32_t strings_offset = 0;
    uint32_t strings_size = 0;
    uint32_t index_crc = 0;
};

// Validates everything that does not need the resident block.
inline bool ParseHeader(const uint8_t* p, size_t len, size_t file_size, Header* out) {
    if (len < kHeaderSize || std::memcmp(p, kMagic, 4) != 0 || ReadU16(p + 4) != kVersion ||
        ReadU16(p + 6) != kHeaderSize || ReadU16(p + 8) != kRecordSize)
        return false;
    Header h;
    h.station_count = ReadU16(p + 10);
    h.region_count = ReadU16(p + 12);
    h.type_count = ReadU16(p + 14);
    h.regions_offset = ReadU32(p + 16);
    h.types_offset = ReadU32(p + 20);
    h.index_offset = ReadU32(p + 24);
    h.strings_offset = ReadU32(p + 28);
    h.strings_size = ReadU32(p + 32);
    h.index_crc = ReadU32(p + 36);
    if (!h.station_count || h.station_count > kMaxStations || !h.region_count ||
        h.region_count > kMaxRegions || h.type_count > kMaxTypes)
        return false;
    if (h.regions_offset != kHeaderSize ||
        h.types_offset != h.regions_offset + 4u * h.region_count ||
        h.index_offset != h.types_offset + 4u * h.type_count ||
        h.strings_offset < h.index_offset + kRecordSize * h.station_count)
        return false;
    if (uint64_t(h.strings_offset) + h.strings_size != file_size)
        return false;
    *out = h;
    return true;
}

inline Record ParseRecord(const uint8_t* p) {
    Record r;
    r.id = ReadU32(p);
    r.strings = ReadU32(p + 4);
    r.heat = ReadU16(p + 8);
    r.types = ReadU16(p + 10);
    r.region = p[12];
    r.name_len = p[13];
    r.url_len = p[14];
    r.py_len = p[15];
    return r;
}

// Checks the NUL layout of one station's strings and points into `buf`.
inline bool SplitStrings(const Record& r, const char* buf, size_t len, Strings* out) {
    if (len != r.StringsBytes() || buf[r.name_len] || buf[r.name_len + 1 + r.url_len] ||
        buf[len - 1])
        return false;
    out->name = buf;
    out->url = buf + r.name_len + 1;
    out->py = out->url + r.url_len + 1;
    return std::strlen(out->name) == r.name_len && std::strlen(out->url) == r.url_len &&
           std::strlen(out->py) == r.py_len;
}

// Higher is better: exact name, name contains, exact pinyin initials, pinyin
// prefix. `needle` must already be ASCII-lowercased.
inline int MatchScore(const char* name, const char* py, const char* needle) {
    if (!needle || !*needle)
        return 0;
    char lowered[256];
    size_t n = 0;
    for (; name[n] && n + 1 < sizeof(lowered); ++n) {
        const unsigned char c = static_cast<unsigned char>(name[n]);
        lowered[n] = c < 0x80 && c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : char(c);
    }
    lowered[n] = 0;
    if (std::strcmp(lowered, needle) == 0)
        return 4;
    if (std::strstr(lowered, needle))
        return 3;
    if (py && *py && std::strcmp(py, needle) == 0)
        return 2;
    if (py && *py && std::strncmp(py, needle, std::strlen(needle)) == 0)
        return 1;
    return 0;
}

// Where a region sits on the dial.
enum class RegionClass : uint8_t { Province, National, Network, Overseas };

inline RegionClass ClassifyRegion(const char* region) {
    if (!region || !*region)
        return RegionClass::Network;
    for (const char* name : {"全国", "中央"})
        if (std::strcmp(region, name) == 0)
            return RegionClass::National;
    if (std::strcmp(region, "网络台") == 0)
        return RegionClass::Network;
    for (const char* name : {"香港", "澳门", "台湾", "新加坡", "马来西亚", "海外"})
        if (std::strcmp(region, name) == 0)
            return RegionClass::Overseas;
    return RegionClass::Province;
}

// Reads the resident block once and serves station strings from the card.
// The file is opened on demand and stays open until CloseFile(), so a burst of
// reads costs one open without pinning a FATFS file slot. Not thread-safe; the
// owner serializes calls.
class Catalog {
public:
    using Alloc = void* (*)(size_t);
    using Free = void (*)(void*);

    Catalog(Alloc alloc, Free free) : alloc_(alloc), free_(free) {}
    ~Catalog() { Close(); }
    Catalog(const Catalog&) = delete;
    Catalog& operator=(const Catalog&) = delete;

    bool Open(const char* path) {
        Close();
        if (!path || std::strlen(path) >= sizeof(path_))
            return false;
        std::strcpy(path_, path);
        file_ = std::fopen(path, "rb");
        if (!file_)
            return false;
        uint8_t raw[kHeaderSize];
        long size = -1;
        if (std::fseek(file_, 0, SEEK_END) == 0)
            size = std::ftell(file_);
        if (size <= 0 || std::fseek(file_, 0, SEEK_SET) != 0 ||
            std::fread(raw, 1, sizeof(raw), file_) != sizeof(raw) ||
            !ParseHeader(raw, sizeof(raw), size_t(size), &header_)) {
            Close();
            return false;
        }
        block_size_ = header_.strings_offset;
        block_ = static_cast<uint8_t*>(alloc_(block_size_));
        if (!block_) {
            Close();
            return false;
        }
        std::memcpy(block_, raw, kHeaderSize);
        const size_t rest = block_size_ - kHeaderSize;
        if (std::fread(block_ + kHeaderSize, 1, rest, file_) != rest ||
            Crc32(block_ + kHeaderSize, rest) != header_.index_crc || !ValidateBlock()) {
            Close();
            return false;
        }
        CloseFile();
        return true;
    }

    void CloseFile() {
        if (file_)
            std::fclose(file_);
        file_ = nullptr;
    }

    void Close() {
        if (file_)
            std::fclose(file_);
        file_ = nullptr;
        if (block_)
            free_(block_);
        block_ = nullptr;
        block_size_ = 0;
        header_ = Header{};
    }

    bool IsOpen() const { return block_ != nullptr; }
    size_t ResidentBytes() const { return block_size_; }
    int Count() const { return block_ ? header_.station_count : 0; }
    int RegionCount() const { return block_ ? header_.region_count : 0; }
    int TypeCount() const { return block_ ? header_.type_count : 0; }

    Record At(int i) const {
        if (!block_ || i < 0 || i >= header_.station_count)
            return Record{};
        return ParseRecord(block_ + header_.index_offset + size_t(i) * kRecordSize);
    }
    const char* RegionName(int region) const {
        if (!block_ || region < 0 || region >= header_.region_count)
            return "";
        return reinterpret_cast<const char*>(block_ +
                                             ReadU32(block_ + header_.regions_offset + 4 * region));
    }
    const char* TypeName(int type) const {
        if (!block_ || type < 0 || type >= header_.type_count)
            return "";
        return reinterpret_cast<const char*>(block_ +
                                             ReadU32(block_ + header_.types_offset + 4 * type));
    }

    // Reads station i's strings into buf (at least kMaxStringsBytes).
    bool ReadStrings(int i, char* buf, size_t cap, Strings* out) {
        const Record r = At(i);
        const size_t len = r.StringsBytes();
        if (!r.id || len > cap || !EnsureFile() ||
            std::fseek(file_, long(header_.strings_offset + r.strings), SEEK_SET) != 0 ||
            std::fread(buf, 1, len, file_) != len)
            return false;
        return SplitStrings(r, buf, len, out);
    }

    // Best MatchScore over every station, reading the string pool in order
    // through `buf`. Returns the index or -1; ties keep the earlier (hotter) one.
    int Find(const char* needle, char* buf, size_t cap) {
        if (cap < kMaxStringsBytes || !EnsureFile())
            return -1;
        int best = -1;
        int best_score = 0;
        size_t window_start = 0;
        size_t window_len = 0;
        for (int i = 0; i < header_.station_count && best_score < 4; ++i) {
            const Record r = At(i);
            const size_t len = r.StringsBytes();
            if (r.strings < window_start || r.strings + len > window_start + window_len) {
                window_start = r.strings;
                const size_t want = header_.strings_size - window_start < cap
                                        ? header_.strings_size - window_start
                                        : cap;
                if (std::fseek(file_, long(header_.strings_offset + window_start), SEEK_SET) != 0)
                    return best;
                window_len = std::fread(buf, 1, want, file_);
                if (len > window_len)
                    return best;
            }
            Strings s;
            if (!SplitStrings(r, buf + (r.strings - window_start), len, &s))
                continue;
            const int score = MatchScore(s.name, s.py, needle);
            if (score > best_score) {
                best_score = score;
                best = i;
            }
        }
        return best;
    }

private:
    // Reopens the card file and checks it is still the catalog we indexed.
    bool EnsureFile() {
        if (file_)
            return true;
        if (!block_)
            return false;
        file_ = std::fopen(path_, "rb");
        uint8_t raw[kHeaderSize];
        if (file_ && std::fread(raw, 1, sizeof(raw), file_) == sizeof(raw) &&
            std::memcmp(raw, block_, kHeaderSize) == 0)
            return true;
        CloseFile();
        return false;
    }

    bool ValidName(uint32_t offset) const {
        if (offset < header_.index_offset + kRecordSize * header_.station_count ||
            offset >= block_size_)
            return false;
        return std::memchr(block_ + offset, 0, block_size_ - offset) != nullptr;
    }

    bool ValidateBlock() const {
        for (int i = 0; i < header_.region_count; ++i)
            if (!ValidName(ReadU32(block_ + header_.regions_offset + 4 * i)))
                return false;
        for (int i = 0; i < header_.type_count; ++i)
            if (!ValidName(ReadU32(block_ + header_.types_offset + 4 * i)))
                return false;
        const uint16_t type_mask =
            header_.type_count >= 16 ? 0xffff : uint16_t((1u << header_.type_count) - 1);
        for (int i = 0; i < header_.station_count; ++i) {
            const Record r = At(i);
            if (!r.id || r.region >= header_.region_count || (r.types & ~type_mask) ||
                !r.name_len || !r.url_len ||
                uint64_t(r.strings) + r.StringsBytes() > header_.strings_size)
                return false;
        }
        return true;
    }

    Alloc alloc_;
    Free free_;
    char path_[96] = {};
    FILE* file_ = nullptr;
    uint8_t* block_ = nullptr;
    size_t block_size_ = 0;
    Header header_;
};

}  // namespace nabo_radio_catalog
