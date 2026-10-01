#include "ir_store.h"

#include <cJSON.h>
#include <esp_log.h>
#include <nvs.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "tab5_sd.h"

namespace ir {
namespace {

const char* TAG = "IrStore";
constexpr const char* kDir = "/sdcard/tab5/ir";
constexpr const char* kFile = "/sdcard/tab5/ir/devices.json";
constexpr const char* kNvsNs = "irstore";
constexpr size_t kNvsReserveEntries = 160;  // leave ~5 KB of NVS for Wi-Fi and settings
constexpr size_t kMaxTimings = 1500;

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

std::string Str(cJSON* obj, const char* key, const char* fallback = "") {
    auto* item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : fallback;
}
int Int(cJSON* obj, const char* key, int fallback) {
    auto* item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

cJSON* DeviceToJson(const Device& d, bool with_timings) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", d.id);
    cJSON_AddStringToObject(o, "name", d.name.c_str());
    cJSON_AddStringToObject(o, "type", d.type.c_str());
    cJSON_AddNumberToObject(o, "carrier", d.carrier_hz);
    cJSON_AddNumberToObject(o, "repeat", d.repeat);
    cJSON* keys = cJSON_AddArrayToObject(o, "keys");
    for (const auto& k : d.keys) {
        cJSON* ko = cJSON_CreateObject();
        cJSON_AddNumberToObject(ko, "id", k.id);
        cJSON_AddStringToObject(ko, "name", k.name.c_str());
        if (with_timings && k.learned()) {
            cJSON* t = cJSON_AddArrayToObject(ko, "t");
            for (uint16_t v : k.timings)
                cJSON_AddItemToArray(t, cJSON_CreateNumber(v));
        }
        cJSON_AddItemToArray(keys, ko);
    }
    return o;
}

bool DeviceFromJson(cJSON* o, Device* d) {
    d->id = Int(o, "id", 0);
    d->name = Str(o, "name");
    d->type = Str(o, "type", "other");
    d->carrier_hz = Int(o, "carrier", 38000);
    d->repeat = std::clamp(Int(o, "repeat", 1), 1, 5);
    d->keys.clear();
    cJSON* keys = cJSON_GetObjectItem(o, "keys");
    cJSON* k = nullptr;
    cJSON_ArrayForEach(k, keys) {
        Key key;
        key.id = Int(k, "id", 0);
        key.name = Str(k, "name");
        cJSON* t = cJSON_GetObjectItem(k, "t");
        cJSON* v = nullptr;
        cJSON_ArrayForEach(v, t) {
            if (key.timings.size() < kMaxTimings && cJSON_IsNumber(v))
                key.timings.push_back(static_cast<uint16_t>(std::clamp(v->valueint, 1, 65535)));
        }
        if (!key.name.empty())
            d->keys.push_back(std::move(key));
    }
    return d->id > 0 && !d->name.empty();
}

const char* TypeLabel(const std::string& type) {
    for (const auto& t : DeviceTypes())
        if (type == t.type)
            return t.label;
    return "设备";
}

std::vector<std::string> DefaultKeys(const std::string& type) {
    if (type == "tv")
        return {"电源", "音量+", "音量-", "静音", "频道+", "频道-", "上", "下", "左", "右", "确定",
                "返回", "主页", "菜单", "信号源"};
    if (type == "ac")
        return {"开机", "关机", "制冷", "制热", "除湿", "送风", "温度+", "温度-", "风速", "摆风",
                "睡眠", "定时"};
    if (type == "box")
        return {"电源", "上", "下", "左", "右", "确定", "返回", "主页", "菜单", "音量+", "音量-",
                "频道+", "频道-"};
    if (type == "fan")
        return {"开关", "风速", "摇头", "定时", "模式"};
    if (type == "projector")
        return {"电源", "上", "下", "左", "右", "确定", "返回", "音量+", "音量-", "信号源"};
    if (type == "light")
        return {"开灯", "关灯", "亮度+", "亮度-", "色温", "夜灯"};
    if (type == "audio")
        return {"电源", "音量+", "音量-", "静音", "播放", "上一曲", "下一曲", "输入"};
    return {"按键1", "按键2", "按键3", "按键4", "按键5", "按键6"};
}

bool Contains(const std::string& hay, const std::string& needle) {
    return !needle.empty() && hay.find(needle) != std::string::npos;
}

}  // namespace

const std::vector<DeviceType>& DeviceTypes() {
    static const std::vector<DeviceType> types = {
        {"tv", "电视"},        {"ac", "空调"},   {"box", "机顶盒"}, {"fan", "风扇"},
        {"projector", "投影仪"}, {"light", "灯"}, {"audio", "音响"}, {"other", "其他"},
    };
    return types;
}

std::vector<std::string> SuggestedKeys(const std::string& type) {
    std::vector<std::string> names = DefaultKeys(type);
    const std::vector<std::string> extra = {
        "电源", "开机", "关机", "上", "下", "左", "右", "确定", "返回", "主页", "菜单",
        "音量+", "音量-", "静音", "频道+", "频道-", "信号源", "播放", "暂停", "快进", "快退",
        "制冷", "制热", "除湿", "送风", "自动", "温度+", "温度-", "风速", "摆风", "睡眠", "定时",
        "灯光", "亮度+", "亮度-", "数字1", "数字2", "数字3", "数字4", "数字5", "数字6", "数字7",
        "数字8", "数字9", "数字0", "按键1", "按键2", "按键3", "按键4", "按键5", "按键6", "按键7",
        "按键8"};
    for (const auto& n : extra)
        if (std::find(names.begin(), names.end(), n) == names.end())
            names.push_back(n);
    return names;
}

Store& Store::GetInstance() {
    static Store store;
    return store;
}

Device* Store::DeviceLocked(int id) {
    for (auto& d : devices_)
        if (d.id == id)
            return &d;
    return nullptr;
}

void Store::Load() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (loaded_)
        return;
    loaded_ = true;
    // The SD card is mounted by a background task shortly after boot; give it a moment so an
    // early caller does not fall back to the (possibly smaller) NVS backup.
    for (int i = 0; i < 30 && !Tab5SdReady(); ++i)
        vTaskDelay(pdMS_TO_TICKS(100));
    if (Tab5SdReady() && LoadSdLocked()) {
        on_sd_ = true;
        ESP_LOGI(TAG, "loaded %u device(s) from SD", unsigned(devices_.size()));
        return;
    }
    on_sd_ = Tab5SdReady();
    if (LoadNvsLocked()) {
        ESP_LOGI(TAG, "loaded %u device(s) from NVS backup", unsigned(devices_.size()));
        if (on_sd_)
            SaveLocked();  // SD card is new/empty: restore the backup onto it
        return;
    }
    MigrateOldSlotsLocked();
}

bool Store::LoadSdLocked() {
    FILE* f = std::fopen(kFile, "rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 4 * 1024 * 1024) {
        std::fclose(f);
        return false;
    }
    std::string text(size_t(size), '\0');
    const size_t got = std::fread(text.data(), 1, text.size(), f);
    std::fclose(f);
    Json root(cJSON_ParseWithLength(text.data(), got), cJSON_Delete);
    if (!root)
        return false;
    devices_.clear();
    next_id_ = std::max(1, Int(root.get(), "next", 1));
    cJSON* d = nullptr;
    cJSON_ArrayForEach(d, cJSON_GetObjectItem(root.get(), "devices")) {
        Device dev;
        if (DeviceFromJson(d, &dev)) {
            next_id_ = std::max(next_id_, dev.id + 1);
            devices_.push_back(std::move(dev));
        }
    }
    return true;
}

bool Store::LoadNvsLocked() {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK)
        return false;
    size_t len = 0;
    bool ok = nvs_get_str(h, "index", nullptr, &len) == ESP_OK && len > 0;
    std::string index(len, '\0');
    ok = ok && nvs_get_str(h, "index", index.data(), &len) == ESP_OK;
    if (ok) {
        Json root(cJSON_Parse(index.c_str()), cJSON_Delete);
        ok = root != nullptr;
        if (ok) {
            devices_.clear();
            next_id_ = std::max(1, Int(root.get(), "next", 1));
            cJSON* d = nullptr;
            cJSON_ArrayForEach(d, cJSON_GetObjectItem(root.get(), "devices")) {
                Device dev;
                if (!DeviceFromJson(d, &dev))
                    continue;
                for (auto& k : dev.keys) {
                    char key[16];
                    std::snprintf(key, sizeof(key), "t%d_%d", dev.id, k.id);
                    size_t blob = 0;
                    if (nvs_get_blob(h, key, nullptr, &blob) == ESP_OK && blob >= 2) {
                        k.timings.resize(blob / 2);
                        if (nvs_get_blob(h, key, k.timings.data(), &blob) != ESP_OK)
                            k.timings.clear();
                    }
                }
                next_id_ = std::max(next_id_, dev.id + 1);
                devices_.push_back(std::move(dev));
            }
        }
    }
    nvs_close(h);
    return ok;
}

void Store::MigrateOldSlotsLocked() {
    // v1.0.x page stored codes in namespace "ir_slots" as s0..s19.
    nvs_handle_t h;
    if (nvs_open("ir_slots", NVS_READONLY, &h) != ESP_OK)
        return;
    struct Old {
        int slot;
        const char* name;
        bool tv;
    };
    static const Old kOld[] = {
        {0, "电源", true},   {1, "音量+", true},  {2, "音量-", true},  {3, "频道+", true},
        {4, "频道-", true},  {5, "确定", true},   {12, "电源", false}, {13, "温度+", false},
        {14, "温度-", false}, {15, "模式", false}, {16, "风速", false}, {17, "摆风", false},
        {18, "睡眠", false},  {19, "灯光", false},
    };
    Device tv{next_id_++, "电视", "tv", 38000, 1, {}};
    Device ac{next_id_++, "空调", "ac", 38000, 1, {}};
    for (const auto& o : kOld) {
        char key[8];
        std::snprintf(key, sizeof(key), "s%d", o.slot);
        size_t len = 0;
        if (nvs_get_blob(h, key, nullptr, &len) != ESP_OK || len < 8)
            continue;
        Key k;
        k.name = o.name;
        k.timings.resize(len / 2);
        if (nvs_get_blob(h, key, k.timings.data(), &len) != ESP_OK)
            continue;
        Device& dev = o.tv ? tv : ac;
        k.id = int(dev.keys.size()) + 1;
        dev.keys.push_back(std::move(k));
    }
    nvs_close(h);
    for (Device* dev : {&tv, &ac}) {
        if (dev->keys.empty())
            continue;
        ESP_LOGI(TAG, "migrated %u learned key(s) into %s", unsigned(dev->keys.size()), dev->name.c_str());
        devices_.push_back(std::move(*dev));
    }
    if (!devices_.empty())
        SaveLocked();
}

void Store::SaveNvsLocked() {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_erase_all(h);
    nvs_commit(h);
    // Names first (always fits), then as many key codes as the shared NVS can spare.
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "next", next_id_);
    cJSON* arr = cJSON_AddArrayToObject(root, "devices");
    for (const auto& d : devices_)
        cJSON_AddItemToArray(arr, DeviceToJson(d, false));
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text) {
        nvs_set_str(h, "index", text);
        cJSON_free(text);
    }
    size_t stored = 0, skipped = 0;
    for (const auto& d : devices_) {
        for (const auto& k : d.keys) {
            if (!k.learned())
                continue;
            nvs_stats_t stats = {};
            const size_t entries = (k.timings.size() * 2 + 31) / 32 + 2;
            if (nvs_get_stats(nullptr, &stats) != ESP_OK ||
                stats.free_entries < kNvsReserveEntries + entries) {
                ++skipped;
                continue;
            }
            char key[16];
            std::snprintf(key, sizeof(key), "t%d_%d", d.id, k.id);
            if (nvs_set_blob(h, key, k.timings.data(), k.timings.size() * 2) == ESP_OK)
                ++stored;
            else
                ++skipped;
        }
    }
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "NVS backup: %u key code(s) stored, %u only on SD", unsigned(stored), unsigned(skipped));
}

void Store::SaveLocked() {
    if (Tab5SdReady()) {
        mkdir("/sdcard/tab5", 0775);
        mkdir(kDir, 0775);
        cJSON* root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "version", 1);
        cJSON_AddNumberToObject(root, "next", next_id_);
        cJSON* arr = cJSON_AddArrayToObject(root, "devices");
        for (const auto& d : devices_)
            cJSON_AddItemToArray(arr, DeviceToJson(d, true));
        char* text = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (text) {
            const std::string tmp = std::string(kFile) + ".tmp";
            FILE* f = std::fopen(tmp.c_str(), "wb");
            const size_t len = std::strlen(text);
            bool ok = f && std::fwrite(text, 1, len, f) == len;
            if (f)
                ok = (std::fclose(f) == 0) && ok;
            cJSON_free(text);
            if (ok) {
                unlink(kFile);
                ok = rename(tmp.c_str(), kFile) == 0;
            }
            on_sd_ = ok;
            if (!ok)
                ESP_LOGE(TAG, "SD save failed");
        }
    }
    SaveNvsLocked();
}

std::vector<Device> Store::Devices() {
    std::lock_guard<std::mutex> lock(mutex_);
    return devices_;
}

bool Store::GetDevice(int id, Device* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* d = DeviceLocked(id);
    if (d && out)
        *out = *d;
    return d != nullptr;
}

int Store::AddDevice(const std::string& type) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (devices_.size() >= 24)
        return -1;
    Device d;
    d.id = next_id_++;
    d.type = type;
    std::string base = TypeLabel(type);
    d.name = base;
    for (int n = 2; std::any_of(devices_.begin(), devices_.end(),
                                [&d](const Device& x) { return x.name == d.name; });
         ++n)
        d.name = base + std::to_string(n);
    int kid = 1;
    for (const auto& name : DefaultKeys(type))
        d.keys.push_back({kid++, name, {}});
    devices_.push_back(std::move(d));
    SaveLocked();
    return devices_.back().id;
}

bool Store::RemoveDevice(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto before = devices_.size();
    devices_.erase(std::remove_if(devices_.begin(), devices_.end(), [id](const Device& d) { return d.id == id; }),
                   devices_.end());
    if (devices_.size() == before)
        return false;
    SaveLocked();
    return true;
}

int Store::AddKey(int device_id, const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* d = DeviceLocked(device_id);
    if (!d || d->keys.size() >= 60 || name.empty())
        return -1;
    for (const auto& k : d->keys)
        if (k.name == name)
            return k.id;
    int id = 1;
    for (const auto& k : d->keys)
        id = std::max(id, k.id + 1);
    d->keys.push_back({id, name, {}});
    SaveLocked();
    return id;
}

bool Store::RemoveKey(int device_id, int key_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* d = DeviceLocked(device_id);
    if (!d)
        return false;
    const auto before = d->keys.size();
    d->keys.erase(std::remove_if(d->keys.begin(), d->keys.end(), [key_id](const Key& k) { return k.id == key_id; }),
                  d->keys.end());
    if (d->keys.size() == before)
        return false;
    SaveLocked();
    return true;
}

bool Store::SetTimings(int device_id, int key_id, const std::vector<uint16_t>& timings) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* d = DeviceLocked(device_id);
    if (!d)
        return false;
    for (auto& k : d->keys) {
        if (k.id == key_id) {
            k.timings = timings;
            if (k.timings.size() > kMaxTimings)
                k.timings.resize(kMaxTimings);
            SaveLocked();
            return true;
        }
    }
    return false;
}

bool Store::GetTimings(int device_id, int key_id, std::vector<uint16_t>* timings, uint32_t* carrier,
                       int* repeat) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* d = DeviceLocked(device_id);
    if (!d)
        return false;
    for (const auto& k : d->keys) {
        if (k.id == key_id && k.learned()) {
            if (timings)
                *timings = k.timings;
            if (carrier)
                *carrier = d->carrier_hz;
            if (repeat)
                *repeat = d->repeat;
            return true;
        }
    }
    return false;
}

bool Store::Find(const std::string& device, const std::string& key, int* device_id, int* key_id,
                 std::string* resolved) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Aliases so "打开空调" works with keys named 开机/电源.
    std::vector<std::string> wanted = {key};
    if (key == "开" || key == "打开" || key == "开机")
        wanted = {"开机", "电源", "开关", "开灯"};
    else if (key == "关" || key == "关闭" || key == "关机")
        wanted = {"关机", "电源", "开关", "关灯"};
    else if (key == "OK" || key == "ok")
        wanted = {"确定"};
    int best = -1;
    const Device* best_dev = nullptr;
    const Key* best_key = nullptr;
    for (const auto& d : devices_) {
        int dscore = 0;
        if (device.empty())
            dscore = 1;
        else if (d.name == device)
            dscore = 4;
        else if (Contains(device, d.name) || Contains(d.name, device))
            dscore = 3;
        else if (Contains(device, TypeLabel(d.type)))
            dscore = 2;
        if (!dscore)
            continue;
        for (const auto& k : d.keys) {
            if (!k.learned())
                continue;
            for (size_t w = 0; w < wanted.size(); ++w) {
                int kscore = 0;
                if (k.name == wanted[w])
                    kscore = 30 - int(w);
                else if (Contains(k.name, wanted[w]) || Contains(wanted[w], k.name))
                    kscore = 10 - int(w);
                if (kscore > 0 && dscore * 100 + kscore > best) {
                    best = dscore * 100 + kscore;
                    best_dev = &d;
                    best_key = &k;
                }
            }
        }
    }
    if (!best_dev)
        return false;
    *device_id = best_dev->id;
    *key_id = best_key->id;
    if (resolved)
        *resolved = best_dev->name + "·" + best_key->name;
    return true;
}

std::string Store::SummaryJson() {
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON* arr = cJSON_CreateArray();
    for (const auto& d : devices_) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "device", d.name.c_str());
        cJSON* learned = cJSON_AddArrayToObject(o, "learned_keys");
        for (const auto& k : d.keys)
            if (k.learned())
                cJSON_AddItemToArray(learned, cJSON_CreateString(k.name.c_str()));
        cJSON_AddItemToArray(arr, o);
    }
    char* text = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    std::string out = text ? text : "[]";
    if (text)
        cJSON_free(text);
    return out;
}

Config Store::GetConfig() {
    Config c;
    nvs_handle_t h;
    if (nvs_open("irconf", NVS_READONLY, &h) == ESP_OK) {
        int32_t v = 0;
        if (nvs_get_i32(h, "tx", &v) == ESP_OK)
            c.tx_gpio = v;
        if (nvs_get_i32(h, "rx", &v) == ESP_OK)
            c.rx_gpio = v;
        if (nvs_get_i32(h, "pol", &v) == ESP_OK) {
            c.active_high = v != 0;
            c.polarity_checked = true;
        }
        nvs_close(h);
    }
    return c;
}

void Store::SetConfig(const Config& c) {
    nvs_handle_t h;
    if (nvs_open("irconf", NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_i32(h, "tx", c.tx_gpio);
    nvs_set_i32(h, "rx", c.rx_gpio);
    nvs_set_i32(h, "pol", c.active_high ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

}  // namespace ir
