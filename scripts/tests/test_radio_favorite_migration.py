"""Exercise the real favorite migration with NVS write failures and retries."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "main/boards/qdtech/tab5/radio_service.cc"


def harness():
    source = SOURCE.read_text()
    helpers = source[source.index("static constexpr uint32_t kIdBlobVersion"):
                     source.index("// Must run on a task with an internal-RAM stack")]
    migration = source[source.index("void RadioService::LoadFavorites()"):
                       source.index("void RadioService::SaveFavorites()")]
    return r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using nvs_handle_t = int;
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, NVS_READONLY = 0, NVS_READWRITE = 1;
constexpr const char* TAG = "test";
constexpr const char* kFavoritesNamespace = "radio_fav";
constexpr const char* kStationNamespace = "radio_st";
constexpr size_t kMaxFavorites = 200, kMaxRecent = 10;
std::mutex g_favorites_mutex;
std::vector<uint32_t> g_favorite_ids, g_recent_ids;
std::map<std::string, std::vector<uint32_t>> stored, pending;
std::map<std::string, int32_t> legacy;
std::string failure;
int erased = 0, opened = 0, closed = 0;

template <typename... Args> void Log(Args&&...) {}
#define ESP_LOGW(...) Log(__VA_ARGS__)
#define ESP_LOGI(...) Log(__VA_ARGS__)
const char* esp_err_to_name(esp_err_t) { return "injected failure"; }
std::string Key(nvs_handle_t h, const char* key) {
    return std::to_string(h) + "/" + key;
}
esp_err_t nvs_open(const char* ns, int mode, nvs_handle_t* handle) {
    if (mode == NVS_READWRITE && failure == "open") return ESP_FAIL;
    *handle = std::strcmp(ns, kFavoritesNamespace) == 0 ? 1 : 2;
    ++opened;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char* key, void* out, size_t* bytes) {
    const auto it = stored.find(Key(h, key));
    if (it == stored.end()) return ESP_FAIL;
    const size_t size = it->second.size() * sizeof(uint32_t);
    if (out && *bytes < size) return ESP_FAIL;
    if (out) std::memcpy(out, it->second.data(), size);
    *bytes = size;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char* key, const void* data, size_t bytes) {
    if (failure == "set") return ESP_FAIL;
    const auto* ids = static_cast<const uint32_t*>(data);
    pending[Key(h, key)] = std::vector<uint32_t>(ids, ids + bytes / sizeof(uint32_t));
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t) {
    if (failure == "commit") return ESP_FAIL;
    for (const auto& item : pending) stored[item.first] = item.second;
    pending.clear();
    return ESP_OK;
}
void nvs_close(nvs_handle_t) { ++closed; }

class Settings {
public:
    Settings(const char*, bool) {}
    int32_t GetInt(const char* key, int32_t fallback) {
        const auto it = legacy.find(key);
        return it == legacy.end() ? fallback : it->second;
    }
    void EraseKey(const char* key) { erased += int(legacy.erase(key)); }
};
std::vector<uint32_t> LegacyStationIds() { return {15318317, 1254, 15318317}; }
class RadioService {
public:
    void LoadFavorites();
};
''' + helpers + migration + r'''
int main(int argc, char** argv) {
    assert(argc == 2);
    failure = argv[1];
    legacy = {{"fav_0", 1}, {"fav_1", 0}, {"fav_2", 1}};
    stored["2/recent"] = {1, 1254};
    if (failure == "existing") stored["1/ids"] = {1, 1254};
    RadioService radio;
    radio.LoadFavorites();
    assert((g_recent_ids == std::vector<uint32_t>{1254}));
    assert(opened == closed);
    if (failure == "existing") {
        assert((g_favorite_ids == std::vector<uint32_t>{1254}));
        assert(legacy.size() == 3 && erased == 0 && pending.empty());
        return 0;
    }
    assert((g_favorite_ids == std::vector<uint32_t>{15318317}));
    if (failure != "success") {
        // A failed save keeps both selected and unselected legacy keys. The
        // in-memory favorites still work, and a subsequent boot can retry.
        assert(legacy.size() == 3 && erased == 0);
        assert(stored.find("1/ids") == stored.end());
        pending.clear();
        failure.clear();
        radio.LoadFavorites();
    }
    assert(legacy.empty() && erased == 3);
    assert((stored["1/ids"] == std::vector<uint32_t>{1, 15318317}));
    assert((g_favorite_ids == std::vector<uint32_t>{15318317}));
    assert(opened == closed);
}
'''


@unittest.skipUnless(shutil.which("c++"), "C++ host compiler required")
class RadioFavoriteMigrationTests(unittest.TestCase):
    def test_failed_save_preserves_legacy_favorites_and_retries(self):
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "migration.cc"
            binary = Path(directory) / "migration"
            cpp.write_text(harness())
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(cpp), "-o", str(binary)], check=True)
            for scenario in ("open", "set", "commit", "success", "existing"):
                with self.subTest(scenario=scenario):
                    subprocess.run([str(binary), scenario], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
