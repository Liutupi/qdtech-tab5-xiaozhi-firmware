#include "tab5_music_lyrics.h"

#include <esp_log.h>
#include <cJSON.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <memory>
#include <string>

#include "board.h"
#include "http.h"
#include "tab5_lrc.h"

namespace tab5_music_lyrics {
namespace {

constexpr size_t kMaxJsonBytes = 48 * 1024;
constexpr size_t kMaxLrcBytes = 16 * 1024;
constexpr int kTimeoutMs = 5000;
const char* TAG = "Tab5MusicLyrics";

std::string UrlEncode(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size() * 3);
    for (unsigned char ch : value) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            result.push_back(static_cast<char>(ch));
        } else {
            result.push_back('%');
            result.push_back(hex[ch >> 4]);
            result.push_back(hex[ch & 15]);
        }
    }
    return result;
}

std::string GetJson(const std::string& url) {
    auto network = Board::GetInstance().GetNetwork();
    if (!network)
        return {};
    auto http = network->CreateHttp(0);
    if (!http)
        return {};
    http->SetTimeout(kTimeoutMs);
    http->SetHeader("Accept", "application/json");
    http->SetHeader("User-Agent", "Mozilla/5.0");
    http->SetHeader("Referer", "https://music.163.com/");
    auto opened = http->Open("GET", url);
    if (!opened)
        return {};
    const auto status = http->GetStatusCode();
    if (!status || *status != 200 || http->GetBodyLength() > kMaxJsonBytes) {
        http->Close();
        return {};
    }
    std::string body;
    body.reserve(std::min<size_t>(http->GetBodyLength(), kMaxJsonBytes));
    char buffer[1024];
    while (body.size() <= kMaxJsonBytes) {
        auto read = http->Read(buffer, sizeof(buffer));
        if (!read) {
            body.clear();
            break;
        }
        if (*read == 0)
            break;
        if (body.size() + *read > kMaxJsonBytes) {
            body.clear();
            break;
        }
        body.append(buffer, *read);
    }
    http->Close();
    return body;
}

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

// Lower-case ASCII and drop spaces/punctuation so "Enough To Be Heard" matches
// "enough to be heard" and full-width/half-width spacing differences don't matter.
std::string Normalize(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char ch : text) {
        if (ch < 0x80) {
            if (std::isalnum(ch))
                out.push_back(char(std::tolower(ch)));
            continue;
        }
        out.push_back(char(ch));
    }
    return out;
}

std::vector<std::string> SplitArtists(const std::string& artist) {
    std::vector<std::string> out;
    std::string current;
    auto flush = [&] {
        auto n = Normalize(current);
        if (!n.empty())
            out.push_back(n);
        current.clear();
    };
    for (size_t i = 0; i < artist.size(); ++i) {
        const char ch = artist[i];
        if (ch == ',' || ch == '/' || ch == '&' || ch == ';') {
            flush();
            continue;
        }
        // "、" (E3 80 81) and "，" (EF BC 8C)
        if (i + 2 < artist.size() &&
            ((uint8_t(ch) == 0xE3 && uint8_t(artist[i + 1]) == 0x80 &&
              uint8_t(artist[i + 2]) == 0x81) ||
             (uint8_t(ch) == 0xEF && uint8_t(artist[i + 1]) == 0xBC &&
              uint8_t(artist[i + 2]) == 0x8C))) {
            flush();
            i += 2;
            continue;
        }
        current.push_back(ch);
    }
    flush();
    return out;
}

std::string FindSongId(const std::string& title, const std::string& artist) {
    if (title.empty() || title.size() > 180 || artist.size() > 180)
        return {};
    const std::string url =
        "https://music.163.com/api/search/get?s=" + UrlEncode(title + " " + artist) +
        "&type=1&limit=5";
    const std::string response = GetJson(url);
    if (response.empty()) {
        ESP_LOGW(TAG, "Song search unavailable");
        return {};
    }
    Json root(cJSON_ParseWithLength(response.data(), response.size()), cJSON_Delete);
    if (!root)
        return {};
    auto* code = cJSON_GetObjectItem(root.get(), "code");
    if (!cJSON_IsNumber(code) || code->valueint != 200)
        return {};
    auto* result = cJSON_GetObjectItem(root.get(), "result");
    auto* songs = cJSON_GetObjectItem(result, "songs");
    if (!cJSON_IsArray(songs))
        return {};
    // When an artist was supplied, a title-only result can belong to another
    // recording of the same song. Use the relaxed passes only when there is no
    // artist to check.
    const std::string want_title = Normalize(title);
    const auto want_artists = SplitArtists(artist);
    if (!artist.empty() && want_artists.empty())
        return {};
    const int passes = artist.empty() ? 3 : 1;
    for (int pass = 0; pass < passes; ++pass) {
        cJSON* song = nullptr;
        cJSON_ArrayForEach (song, songs) {
            auto* name = cJSON_GetObjectItem(song, "name");
            auto* id = cJSON_GetObjectItem(song, "id");
            auto* artists = cJSON_GetObjectItem(song, "artists");
            if (!cJSON_IsString(name) || !cJSON_IsNumber(id))
                continue;
            const std::string got_title = Normalize(name->valuestring);
            bool ok = false;
            if (pass == 0 || pass == 1) {
                ok = got_title == want_title;
                if (ok && pass == 0) {
                    bool artist_matches = want_artists.empty();
                    cJSON* candidate = nullptr;
                    if (cJSON_IsArray(artists)) {
                        cJSON_ArrayForEach (candidate, artists) {
                            auto* artist_name = cJSON_GetObjectItem(candidate, "name");
                            if (!cJSON_IsString(artist_name))
                                continue;
                            const std::string got = Normalize(artist_name->valuestring);
                            for (const auto& want : want_artists)
                                artist_matches |= got == want;
                        }
                    }
                    ok = artist_matches;
                }
            } else {
                ok = !want_title.empty() && got_title.find(want_title) != std::string::npos;
            }
            if (!ok)
                continue;
            char id_text[24];
            std::snprintf(id_text, sizeof(id_text), "%.0f", id->valuedouble);
            ESP_LOGI(TAG, "Song matched on pass %d id=%s", pass, id_text);
            return id_text;
        }
    }
    return {};
}

bool ValidSongId(const std::string& id) {
    return !id.empty() && id.size() <= 18 &&
           std::all_of(id.begin(), id.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
}

std::string FetchTimedLrc(const std::string& id) {
    const auto response =
        GetJson("https://music.163.com/api/song/lyric?id=" + id + "&lv=1&kv=0&tv=-1");
    if (response.empty()) {
        ESP_LOGW(TAG, "Lyric response unavailable id=%s", id.c_str());
        return {};
    }
    Json root(cJSON_ParseWithLength(response.data(), response.size()), cJSON_Delete);
    if (!root)
        return {};
    auto* lrc = cJSON_GetObjectItem(root.get(), "lrc");
    auto* lyric = cJSON_GetObjectItem(lrc, "lyric");
    if (!cJSON_IsString(lyric)) {
        ESP_LOGI(TAG, "No lrc field id=%s (response %u bytes)", id.c_str(),
                 unsigned(response.size()));
        return {};
    }
    std::string text = lyric->valuestring;
    if (text.size() > kMaxLrcBytes) {
        // Keep whole lines only; the parser caps the line count anyway.
        const size_t cut = text.rfind('\n', kMaxLrcBytes);
        text.resize(cut == std::string::npos ? 0 : cut);
    }
    tab5_lrc::Document parsed;
    if (!tab5_lrc::Parse(text, parsed) || !parsed.timed) {
        ESP_LOGI(TAG, "Timed LRC unavailable id=%s lyric_bytes=%u", id.c_str(),
                 unsigned(lyric->valuestring ? std::strlen(lyric->valuestring) : 0));
        return {};
    }
    return text;
}

}  // namespace

std::string Lookup(const std::string& title, const std::string& artist,
                   const std::string& song_id) {
    ESP_LOGI(TAG, "Looking up NetEase lyrics title_bytes=%u artist_bytes=%u",
             unsigned(title.size()), unsigned(artist.size()));
    std::string text;
    if (ValidSongId(song_id)) {
        // A known song ID is authoritative. Searching by title after a missing LRC can
        // select another recording with the same title and display incorrect lyrics.
        text = FetchTimedLrc(song_id);
        if (text.empty())
            return {};
    }
    if (text.empty()) {
        const std::string id = FindSongId(title, artist);
        if (!ValidSongId(id)) {
            ESP_LOGI(TAG, "No NetEase title/artist match");
            return {};
        }
        if (id != song_id)
            text = FetchTimedLrc(id);
    }
    if (text.empty())
        return {};
    tab5_lrc::Document parsed;
    tab5_lrc::Parse(text, parsed);
    ESP_LOGI(TAG, "Timed lyrics ready: %u lines", unsigned(parsed.lines.size()));
    return text;
}

}  // namespace tab5_music_lyrics
