#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Learned IR devices and keys. Kept on the SD card (/sdcard/tab5/ir/devices.json, no size
// limit) with a backup in NVS (names + as many key codes as the small NVS partition can
// hold), so the remote still works when the SD card is missing.
namespace ir {

struct Key {
    int id = 0;
    std::string name;
    std::vector<uint16_t> timings;  // [mark, space, ...] in microseconds
    bool learned() const { return !timings.empty(); }
};

struct Device {
    int id = 0;
    std::string name;
    std::string type;  // tv, ac, box, fan, projector, light, audio, other
    uint32_t carrier_hz = 38000;
    int repeat = 1;
    std::vector<Key> keys;
};

struct Config {
    int tx_gpio = 53;
    int rx_gpio = 54;
    bool active_high = true;
    bool polarity_checked = false;
};

struct DeviceType {
    const char* type;
    const char* label;
};
const std::vector<DeviceType>& DeviceTypes();
// Suggested key names when adding a key to a device of `type`.
std::vector<std::string> SuggestedKeys(const std::string& type);

class Store {
public:
    static Store& GetInstance();

    void Load();  // call once SD is mounted (or after a timeout without SD)
    bool loaded() const { return loaded_; }
    bool on_sd() const { return on_sd_; }

    std::vector<Device> Devices();  // copy (timings included)
    bool GetDevice(int id, Device* out);
    int AddDevice(const std::string& type);  // returns new id, -1 on failure
    bool RemoveDevice(int id);
    int AddKey(int device_id, const std::string& name);  // returns key id
    bool RemoveKey(int device_id, int key_id);
    bool SetTimings(int device_id, int key_id, const std::vector<uint16_t>& timings);
    bool GetTimings(int device_id, int key_id, std::vector<uint16_t>* timings, uint32_t* carrier,
                    int* repeat);

    // Fuzzy lookup by Chinese names ("空调", "开机"); device may be empty (first match).
    bool Find(const std::string& device, const std::string& key, int* device_id, int* key_id,
              std::string* resolved);
    std::string SummaryJson();  // device + learned key names for the assistant

    Config GetConfig();
    void SetConfig(const Config& config);

private:
    Store() = default;
    void SaveLocked();
    bool LoadSdLocked();
    bool LoadNvsLocked();
    void SaveNvsLocked();
    void MigrateOldSlotsLocked();
    Device* DeviceLocked(int id);

    std::mutex mutex_;
    std::vector<Device> devices_;
    int next_id_ = 1;
    bool loaded_ = false;
    bool on_sd_ = false;
};

}  // namespace ir
