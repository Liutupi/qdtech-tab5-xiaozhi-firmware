#include "tab5_sky_weather.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstdlib>
#include <ctime>

#include "settings.h"
#include "tab5_weather_obs.h"

namespace tab5_sky {
namespace {
const char* TAG = "NaboSky";
}  // namespace

WeatherService& WeatherService::GetInstance() {
    static WeatherService instance;
    return instance;
}

void WeatherService::Start() {
    if (task_)
        return;
    {
        Settings settings("weather_cfg", false);
        const double lat = std::atof(settings.GetString("lat", "22.5176").c_str());
        const double lon = std::atof(settings.GetString("lon", "113.3928").c_str());
        if (lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180 && (lat != 0 || lon != 0)) {
            latitude_ = lat;
            longitude_ = lon;
        }
    }
    if (xTaskCreatePinnedToCoreWithCaps(TaskEntry, "nabo_sky", 6144, this, 1, &task_, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        task_ = nullptr;
        ESP_LOGE(TAG, "weather task create failed");
    }
}

// Sunrise/sunset are computed here for today's date, so they are right from the first
// boot minute without any network request.
WeatherNow WeatherService::Current() {
    WeatherNow now;
    double lat, lon;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        now = now_;
        lat = latitude_;
        lon = longitude_;
    }
    const time_t t = time(nullptr);
    struct tm local;
    localtime_r(&t, &local);
    if (local.tm_year + 1900 >= 2025) {
        // newlib's struct tm has no tm_gmtoff: derive the offset from UTC via gmtime/mktime.
        struct tm utc;
        gmtime_r(&t, &utc);
        utc.tm_isdst = 0;
        const double tz_hours = difftime(t, mktime(&utc)) / 3600.0;
        const auto sun = tab5_weather::ComputeSun(lat, lon, local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                                                  tz_hours);
        now.sunrise_min = sun.sunrise_min;
        now.sunset_min = sun.sunset_min;
    }
    return now;
}

void WeatherService::Publish(int wmo_code) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (now_.valid && now_.wmo_code == wmo_code)
        return;
    now_.valid = true;
    now_.wmo_code = wmo_code;
    ++now_.version;
    ESP_LOGI(TAG, "weather code=%d (from the home weather service)", wmo_code);
}

void WeatherService::TaskEntry(void* arg) { static_cast<WeatherService*>(arg)->Run(); }

bool WeatherService::Post(void (*fn)(void*), void* arg) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!task_ || job_)
            return false;
        job_ = fn;
        job_arg_ = arg;
    }
    xTaskNotifyGive(task_);
    return true;
}

void WeatherService::Run() {
    // Only runs posted jobs (sky rendering); weather arrives through Publish().
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        void (*fn)(void*) = nullptr;
        void* arg = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fn = job_;
            arg = job_arg_;
        }
        if (fn) {
            fn(arg);
            std::lock_guard<std::mutex> lock(mutex_);
            job_ = nullptr;
        }
    }
}

}  // namespace tab5_sky
