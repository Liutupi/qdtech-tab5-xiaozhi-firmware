"""Compile the real NetEase title/artist matcher with deterministic search results."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/qdtech/tab5"
SOURCE = BOARD / "tab5_music_lyrics.cc"


def between(source: str, start: str, end: str) -> str:
    return source[source.index(start):source.index(end, source.index(start))]


def harness(source: str) -> str:
    functions = "\n".join([
        between(source, "std::string UrlEncode(", "std::string GetJson("),
        between(source, "std::string Normalize(", "std::vector<std::string> SplitArtists("),
        between(source, "std::vector<std::string> SplitArtists(", "std::string FindSongId("),
        between(source, "std::string FindSongId(", "bool ValidSongId("),
        between(source, "bool ValidSongId(", "std::string FetchTimedLrc("),
    ])
    lookup = between(source, "std::string Lookup(", "}  // namespace tab5_music_lyrics")
    return r'''
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "tab5_lrc.h"

#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)

struct Candidate { std::string title, artist; int id; };
std::vector<Candidate> g_candidates;
std::unordered_map<std::string, std::string> g_lyrics;
std::vector<std::string> g_fetched_ids;
int g_searches = 0;

struct cJSON {
    enum Type { Object, Array, String, Number } type = Object;
    std::string key, text;
    char* valuestring = nullptr;
    double valuedouble = 0;
    int valueint = 0;
    cJSON* child = nullptr;
    cJSON* next = nullptr;
};

cJSON* Add(cJSON* parent, cJSON* child) {
    if (!parent->child) parent->child = child;
    else {
        cJSON* tail = parent->child;
        while (tail->next) tail = tail->next;
        tail->next = child;
    }
    return child;
}
cJSON* Node(cJSON::Type type, const char* key = "") {
    auto* node = new cJSON;
    node->type = type;
    node->key = key;
    return node;
}
cJSON* Text(const char* key, const std::string& value) {
    auto* node = Node(cJSON::String, key);
    node->text = value;
    node->valuestring = node->text.data();
    return node;
}
cJSON* Number(const char* key, int value) {
    auto* node = Node(cJSON::Number, key);
    node->valueint = value;
    node->valuedouble = value;
    return node;
}
cJSON* cJSON_ParseWithLength(const char*, size_t) {
    auto* root = Node(cJSON::Object);
    Add(root, Number("code", 200));
    auto* result = Add(root, Node(cJSON::Object, "result"));
    auto* songs = Add(result, Node(cJSON::Array, "songs"));
    for (const auto& candidate : g_candidates) {
        auto* song = Add(songs, Node(cJSON::Object));
        Add(song, Text("name", candidate.title));
        Add(song, Number("id", candidate.id));
        auto* artists = Add(song, Node(cJSON::Array, "artists"));
        Add(Add(artists, Node(cJSON::Object)), Text("name", candidate.artist));
    }
    return root;
}
void cJSON_Delete(cJSON* node) {
    if (!node) return;
    cJSON_Delete(node->child);
    cJSON_Delete(node->next);
    delete node;
}
cJSON* cJSON_GetObjectItem(cJSON* parent, const char* key) {
    for (auto* node = parent ? parent->child : nullptr; node; node = node->next)
        if (node->key == key) return node;
    return nullptr;
}
bool cJSON_IsNumber(cJSON* node) { return node && node->type == cJSON::Number; }
bool cJSON_IsString(cJSON* node) { return node && node->type == cJSON::String; }
bool cJSON_IsArray(cJSON* node) { return node && node->type == cJSON::Array; }
#define cJSON_ArrayForEach(item, array) \
    for (item = (array) ? (array)->child : nullptr; item; item = item->next)

namespace tab5_music_lyrics {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
std::string GetJson(const std::string& url) {
    if (url.find("/api/search/get?") == std::string::npos) std::abort();
    ++g_searches;
    return "search fixture";
}
''' + functions + r'''
std::string FetchTimedLrc(const std::string& id) {
    g_fetched_ids.push_back(id);
    const auto found = g_lyrics.find(id);
    return found == g_lyrics.end() ? std::string() : found->second;
}
}  // namespace
''' + lookup + r'''
}  // namespace tab5_music_lyrics

void Reset() {
    g_candidates.clear();
    g_lyrics.clear();
    g_fetched_ids.clear();
    g_searches = 0;
}
int main() {
    using tab5_music_lyrics::Lookup;
    // Search results with the right title but wrong artist must not provide LRC.
    Reset();
    g_candidates = {{"同名歌", "乙歌手", 101}};
    g_lyrics["101"] = "[00:01.00]wrong artist\n";
    if (!Lookup("同名歌", "甲歌手", "").empty() || g_searches != 1 ||
        !g_fetched_ids.empty()) return 1;

    // An exact title+artist later in the results is selected, not the first title.
    Reset();
    g_candidates = {{"同名歌", "乙歌手", 101}, {"同名歌", "甲歌手", 202}};
    g_lyrics["101"] = "[00:01.00]wrong artist\n";
    g_lyrics["202"] = "[00:01.00]right artist\n";
    if (Lookup("同名歌", "甲歌手", "") != g_lyrics["202"] ||
        g_fetched_ids != std::vector<std::string>{"202"}) return 2;

    // A trusted numeric song_id uses its own timed LRC without a title search.
    Reset();
    g_lyrics["303"] = "[00:01.00]direct ID\n";
    if (Lookup("other title", "甲歌手", "303") != g_lyrics["303"] ||
        g_searches != 0 || g_fetched_ids != std::vector<std::string>{"303"}) return 3;

    // A trusted ID with no timed LRC must not show another recording's lyrics.
    Reset();
    g_candidates = {{"同名歌", "乙歌手", 101}, {"同名歌", "甲歌手", 202}};
    g_lyrics["202"] = "[00:01.00]right fallback\n";
    if (!Lookup("同名歌", "甲歌手", "999").empty() ||
        g_searches != 0 || g_fetched_ids != std::vector<std::string>({"999"})) return 4;

    // No artist was supplied: preserve the existing title-only fallback.
    Reset();
    g_candidates = {{"同名歌", "乙歌手", 101}};
    g_lyrics["101"] = "[00:01.00]title fallback\n";
    if (Lookup("同名歌", "", "") != g_lyrics["101"]) return 5;

    // Punctuation-only artist text is still a supplied artist, not permission
    // to use a title-only result.
    Reset();
    g_candidates = {{"同名歌", "乙歌手", 101}};
    g_lyrics["101"] = "[00:01.00]wrong artist\n";
    if (!Lookup("同名歌", ",", "").empty()) return 6;
}
'''


class Tab5MusicLyricsMatchTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("c++"), "C++ host compiler required")
    def test_same_title_artist_guard_and_direct_song_id(self):
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "music_lyrics_match.cc"
            binary = Path(directory) / "music_lyrics_match"
            cpp.write_text(harness(SOURCE.read_text()))
            subprocess.run(
                ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(BOARD),
                 str(cpp), str(BOARD / "tab5_lrc.cc"), "-o", str(binary)],
                check=True, capture_output=True, text=True,
            )
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
