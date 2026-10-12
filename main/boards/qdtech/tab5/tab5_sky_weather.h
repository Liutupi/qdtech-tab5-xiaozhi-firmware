#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <mutex>
#include <string>

// Current weather and today's sunrise/sunset for the sky behind Nabo: the CMA station
// observation (www.nmc.cn, plain HTTP, no key) with the ECMWF model via Open-Meteo as a
// fallback, every 20 minutes on a small PSRAM-stack task (which also runs sky jobs).
// Sunrise/sunset are computed locally from the "weather_cfg" location (default Zhongshan).
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
    // Any task: a WMO weather code from another source (e.g. TimeWeatherService builds).
    void Publish(int wmo_code);
    // SNTP callback: the clock is trustworthy from now on (before that, the sky stays day).
    void MarkClockSynced() { clock_synced_.store(true); }
    bool ClockSynced() const { return clock_synced_.load(); }
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
    std::string station_;  // CMA station code, empty = Open-Meteo only
    std::atomic<bool> clock_synced_{false};
    TaskHandle_t task_ = nullptr;
    void (*job_)(void*) = nullptr;
    void* job_arg_ = nullptr;
};

}  // namespace tab5_sky
