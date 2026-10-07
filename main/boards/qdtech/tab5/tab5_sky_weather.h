#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <mutex>

// Current weather and today's sunrise/sunset for the sky behind Nabo (Open-Meteo,
// plain HTTP, no key). Location comes from the existing "weather_cfg" NVS settings
// (default Zhongshan). Refreshes every 30 minutes on its own small PSRAM-stack task.
namespace tab5_sky {

struct WeatherNow {
    bool valid = false;
    int wmo_code = -1;      // WMO weather code
    int sunrise_min = -1;   // local minutes since midnight
    int sunset_min = -1;
    uint32_t version = 0;   // increments on every successful refresh
};

class WeatherService {
public:
    static WeatherService& GetInstance();
    // Call from a normal (internal-RAM stack) task: reads NVS, then starts polling.
    void Start();
    WeatherNow Current();
    // Runs fn(arg) once on the weather task (CPU-heavy sky rendering off the LVGL task).
    // The job must not touch NVS/flash: the task stack is in PSRAM. One job at a time;
    // returns false when the task is not running or a job is still pending.
    bool Post(void (*fn)(void*), void* arg);

private:
    WeatherService() = default;
    static void TaskEntry(void* arg);
    void Run();
    bool Fetch();

    std::mutex mutex_;
    WeatherNow now_;
    double latitude_ = 22.5176, longitude_ = 113.3928;
    TaskHandle_t task_ = nullptr;
    void (*job_)(void*) = nullptr;
    void* job_arg_ = nullptr;
};

}  // namespace tab5_sky
