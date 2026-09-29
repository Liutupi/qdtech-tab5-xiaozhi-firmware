#include "tab5_music_lyrics.h"

#include <esp_log.h>
#include <cJSON.h>

#include <algorithm>
#include <cstdio>
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
    cJSON* song = nullptr;
    cJSON_ArrayForEach (song, songs) {
        auto* name = cJSON_GetObjectItem(song, "name");
        auto* id = cJSON_GetObjectItem(song, "id");
        auto* artists = cJSON_GetObjectItem(song, "artists");
        if (!cJSON_IsString(name) || title != name->valuestring || !cJSON_IsNumber(id) ||
            !cJSON_IsArray(artists))
            continue;
        bool artist_matches = artist.empty();
        cJSON* candidate = nullptr;
        cJSON_ArrayForEach (candidate, artists) {
            auto* artist_name = cJSON_GetObjectItem(candidate, "name");
            if (cJSON_IsString(artist_name) && artist == artist_name->valuestring)
                artist_matches = true;
        }
        if (!artist_matches)
            continue;
        char id_text[24];
        std::snprintf(id_text, sizeof(id_text), "%.0f", id->valuedouble);
        return id_text;
    }
    return {};
}

bool ValidSongId(const std::string& id) {
    return !id.empty() && id.size() <= 18 &&
           std::all_of(id.begin(), id.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
}

}  // namespace

std::string Lookup(const std::string& title, const std::string& artist,
                   const std::string& song_id) {
    ESP_LOGI(TAG, "Looking up NetEase lyrics title_bytes=%u artist_bytes=%u",
             unsigned(title.size()), unsigned(artist.size()));
    std::string id = song_id;
    if (!ValidSongId(id))
        id = FindSongId(title, artist);
    if (!ValidSongId(id)) {
        ESP_LOGI(TAG, "No exact NetEase title/artist match");
        return {};
    }
    const auto response =
        GetJson("https://music.163.com/api/song/lyric?id=" + id + "&lv=1&kv=0&tv=-1");
    if (response.empty()) {
        ESP_LOGW(TAG, "Lyric response unavailable");
        return {};
    }
    Json root(cJSON_ParseWithLength(response.data(), response.size()), cJSON_Delete);
    if (!root)
        return {};
    auto* lrc = cJSON_GetObjectItem(root.get(), "lrc");
    auto* lyric = cJSON_GetObjectItem(lrc, "lyric");
    if (!cJSON_IsString(lyric))
        return {};
    const std::string text = lyric->valuestring;
    if (text.size() > kMaxLrcBytes)
        return {};
    tab5_lrc::Document parsed;
    if (!tab5_lrc::Parse(text, parsed) || !parsed.timed) {
        ESP_LOGI(TAG, "Timed LRC unavailable");
        return {};
    }
    ESP_LOGI(TAG, "Timed lyrics ready: %u lines", unsigned(parsed.lines.size()));
    return text;
}

}  // namespace tab5_music_lyrics
