#include <algorithm>
#include <atomic>
#include "ir_store.h"
#include "ir_service.h"
#include "tab5_nes_video.h"
namespace tab5_nes_video { bool Active() { return false; } bool Available() { return false; } }
namespace ir {
const std::vector<DeviceType>& DeviceTypes() {
    static const std::vector<DeviceType> t = {{"tv", "电视"}, {"ac", "空调"}, {"box", "机顶盒"}, {"fan", "风扇"},
        {"projector", "投影仪"}, {"light", "灯"}, {"audio", "音响"}, {"other", "其他"}};
    return t;
}
std::vector<std::string> SuggestedKeys(const std::string& type) {
    if (type == "ac") return {"开机", "关机", "制冷", "制热", "除湿", "送风", "温度+", "温度-", "风速", "摆风", "睡眠", "定时"};
    return {"电源", "音量+", "音量-", "静音", "频道+", "频道-", "上", "下", "左", "右", "确定", "返回", "主页", "菜单", "信号源"};
}
static std::vector<Device> Seed() {
    std::vector<Device> d;
    auto mk = [](int id, const char* name, const char* type, std::vector<std::string> keys, int learned) {
        Device dev; dev.id = id; dev.name = name; dev.type = type;
        int k = 1;
        for (auto& n : keys) { Key key; key.id = k; key.name = n; if (k <= learned) key.timings = {9000, 4500, 560, 560}; dev.keys.push_back(key); ++k; }
        return dev;
    };
    d.push_back(mk(1, "客厅电视", "tv", SuggestedKeys("tv"), 15));
    d.push_back(mk(2, "卧室空调", "ac", SuggestedKeys("ac"), 12));
    d.push_back(mk(3, "投影仪", "projector", {"电源", "信号源", "上", "下", "确定", "返回"}, 6));
    return d;
}
Store& Store::GetInstance() { static Store s; return s; }
void Store::Load() { std::lock_guard<std::mutex> l(mutex_); if (!loaded_) { devices_ = Seed(); next_id_ = 4; } loaded_ = true; on_sd_ = true; }
std::vector<Device> Store::Devices() { Load(); return devices_; }
bool Store::GetDevice(int id, Device* out) { Load(); for (auto& d : devices_) if (d.id == id) { *out = d; return true; } return false; }
int Store::AddDevice(const std::string&) { return -1; }
bool Store::RemoveDevice(int) { return false; }
int Store::AddKey(int, const std::string&) { return -1; }
bool Store::RemoveKey(int, int) { return false; }
bool Store::SetTimings(int, int, const std::vector<uint16_t>&) { return true; }
bool Store::GetTimings(int, int, std::vector<uint16_t>* t, uint32_t* c, int* r) { if (t) *t = {9000, 4500}; if (c) *c = 38000; if (r) *r = 1; return true; }
Config Store::GetConfig() { Config c; c.polarity_checked = true; return c; }
void Store::SetConfig(const Config&) {}
}  // namespace ir
bool IrService::Learn(int, int, std::vector<uint16_t>*, std::string*, const std::atomic<bool>*) { return false; }
bool IrService::Send(int, const uint16_t*, size_t, bool, uint32_t, int) { return true; }
bool IrService::LoopbackHeard(int, int, bool) { return true; }
