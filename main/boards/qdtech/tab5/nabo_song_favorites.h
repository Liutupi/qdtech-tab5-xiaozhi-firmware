#pragma once

#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "tab5_podcast_model.h"

// Personal songs are independent of the relay's rolling episode history.
namespace nabo_song_favorites {
using Track = tab5_podcast::Track;
using List = tab5_podcast::TrackList;
using SharedList = std::shared_ptr<const List>;
using Bytes = std::vector<uint8_t, tab5_podcast::PsramAllocator<uint8_t>>;
constexpr size_t kMaxSongs = 100, kTitleBytes = 256, kArtistBytes = 192, kIdBytes = 20;
constexpr size_t kMaxFileBytes = 16 + kMaxSongs * (6 + kTitleBytes + kArtistBytes + kIdBytes);

inline std::shared_ptr<List> MakeList() {
    return std::allocate_shared<List>(tab5_podcast::PsramAllocator<List>());
}
inline bool Same(const Track& a, const Track& b) {
    return a.title == b.title && a.artist == b.artist;
}
inline bool Contains(const List& list, const Track& song) {
    return std::any_of(list.begin(), list.end(),
                       [&](const auto& item) { return Same(item, song); });
}
inline bool TextValid(std::string_view text, size_t max_bytes, bool empty_ok) {
    if (text.size() > max_bytes || (!empty_ok && text.empty()))
        return false;
    for (size_t at = 0; at < text.size();) {
        const uint8_t first = uint8_t(text[at++]);
        if (first < 0x20 || first == 0x7f)
            return false;
        if (first < 0x80)
            continue;
        unsigned count = first >= 0xc2 && first <= 0xdf   ? 1
                         : first >= 0xe0 && first <= 0xef ? 2
                         : first >= 0xf0 && first <= 0xf4 ? 3
                                                          : 0;
        if (!count || at + count > text.size())
            return false;
        uint32_t cp = first & ((1u << (6 - count)) - 1);
        for (unsigned i = 0; i < count; ++i) {
            const uint8_t next = uint8_t(text[at++]);
            if ((next & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (next & 0x3f);
        }
        if ((count == 1 && cp < 0x80) || (count == 2 && cp < 0x800) ||
            (count == 3 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return false;
    }
    return true;
}
inline bool IdValid(std::string_view id) {
    return id.size() <= kIdBytes &&
           std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; });
}
inline bool Valid(const Track& song) {
    return TextValid(song.title, kTitleBytes, false) &&
           TextValid(song.artist, kArtistBytes, true) && IdValid(song.song_id);
}
inline uint32_t Crc(const uint8_t* data, size_t size) {
    uint32_t crc = ~0u;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
inline void Put16(Bytes& out, size_t at, uint16_t n) {
    out[at] = uint8_t(n);
    out[at + 1] = uint8_t(n >> 8);
}
inline uint16_t U16(const uint8_t* p) { return p[0] | uint16_t(p[1]) << 8; }
inline bool Encode(const List& list, Bytes& out) {
    if (list.size() > kMaxSongs)
        return false;
    out.assign(16, 0);
    std::memcpy(out.data(), "NABOFAV1", 8);
    Put16(out, 8, uint16_t(list.size()));
    List seen;
    for (const auto& song : list) {
        if (!Valid(song) || Contains(seen, song))
            return false;
        seen.push_back(song);
        const size_t at = out.size();
        out.resize(at + 6);
        Put16(out, at, uint16_t(song.title.size()));
        Put16(out, at + 2, uint16_t(song.artist.size()));
        Put16(out, at + 4, uint16_t(song.song_id.size()));
        out.insert(out.end(), song.title.begin(), song.title.end());
        out.insert(out.end(), song.artist.begin(), song.artist.end());
        out.insert(out.end(), song.song_id.begin(), song.song_id.end());
    }
    const uint32_t crc = Crc(out.data() + 16, out.size() - 16);
    for (int i = 0; i < 4; ++i)
        out[12 + i] = uint8_t(crc >> (8 * i));
    return true;
}
inline bool Decode(const uint8_t* data, size_t size, List& out) {
    if (!data || size < 16 || size > kMaxFileBytes || std::memcmp(data, "NABOFAV1", 8) ||
        U16(data + 8) > kMaxSongs || U16(data + 10) != 0)
        return false;
    uint32_t crc = 0;
    for (int i = 0; i < 4; ++i)
        crc |= uint32_t(data[12 + i]) << (8 * i);
    if (crc != Crc(data + 16, size - 16))
        return false;
    List next;
    size_t at = 16;
    for (unsigned i = 0; i < U16(data + 8); ++i) {
        if (at + 6 > size)
            return false;
        const size_t title = U16(data + at), artist = U16(data + at + 2), id = U16(data + at + 4);
        at += 6;
        if (title > kTitleBytes || artist > kArtistBytes || id > kIdBytes ||
            at + title + artist + id > size)
            return false;
        Track song{
            tab5_podcast::ToStr({reinterpret_cast<const char*>(data + at), title}),
            tab5_podcast::ToStr({reinterpret_cast<const char*>(data + at + title), artist}),
            tab5_podcast::ToStr({reinterpret_cast<const char*>(data + at + title + artist), id})};
        if (!Valid(song) || Contains(next, song))
            return false;
        next.push_back(std::move(song));
        at += title + artist + id;
    }
    if (at != size)
        return false;
    out = std::move(next);
    return true;
}
inline bool ReadFile(const std::string& path, List& out) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    const bool seek_ok = std::fseek(file, 0, SEEK_END) == 0;
    const long size = seek_ok ? std::ftell(file) : -1;
    Bytes bytes;
    bool ok = size >= 16 && size_t(size) <= kMaxFileBytes;
    if (ok) {
        bytes.resize(size_t(size));
        ok = std::fseek(file, 0, SEEK_SET) == 0 &&
             std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
    }
    std::fclose(file);
    return ok && Decode(bytes.data(), bytes.size(), out);
}
inline bool Exists(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    std::fclose(file);
    return true;
}
// FAT does not replace an existing destination with rename. Retain a complete
// fallback until the fsynced temporary file has become the canonical file.
inline bool SaveFile(const std::string& path, const List& list) {
    Bytes bytes;
    if (!Encode(list, bytes))
        return false;
    const std::string temp = path + ".tmp", backup = path + ".bak";
    FILE* file = std::fopen(temp.c_str(), "wb");
    if (!file)
        return false;
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    if (ok)
        ok = std::fflush(file) == 0 && ::fsync(fileno(file)) == 0;
    if (std::fclose(file) != 0)
        ok = false;
    if (!ok)
        return false;
    const bool had_file = Exists(path);
    if (had_file) {
        std::remove(backup.c_str());
        if (std::rename(path.c_str(), backup.c_str()) != 0)
            return false;
    }
    if (std::rename(temp.c_str(), path.c_str()) != 0) {
        if (had_file)
            std::rename(backup.c_str(), path.c_str());
        return false;
    }
    std::remove(backup.c_str());
    return true;
}
inline bool LoadFile(const std::string& path, List& out) {
    if (ReadFile(path, out) || ReadFile(path + ".tmp", out) || ReadFile(path + ".bak", out))
        return true;
    if (Exists(path) || Exists(path + ".tmp") || Exists(path + ".bak"))
        return false;
    out.clear();
    return true;
}

enum class Change { Added, Removed, Full, Unavailable };
enum class Error { None, Corrupt, Write };
struct Snapshot {
    SharedList songs;
    uint32_t revision = 0;
    bool ready = false, pending = false;
    Error error = Error::None;
};
}  // namespace nabo_song_favorites
