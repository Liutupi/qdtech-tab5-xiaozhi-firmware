#include "tab5_sky_weather.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "board.h"
#include "settings.h"

namespace tab5_sky {
namespace {
const char* TAG = "NaboSky";
constexpr int kRefreshSeconds = 30 * 60;
constexpr int kRetrySeconds = 5 * 60;
constexpr int kFirstDelayMs = 30000;  // let Wi-Fi, SNTP and the XiaoZhi session settle
constexpr size_t kMaxBody = 8 * 1024;

// "2026-10-07T06:13" -> 373; -1 when malformed.
int LocalMinutes(const char* iso) {
    if (!iso)
        return -1;
    const char* t = std::strchr(iso, 'T');
    if (!t || std::strlen(t) < 6)
        return -1;
    const int h = std::atoi(t + 1), m = std::atoi(t + 4);
    return (h >= 0 && h < 24 && m >= 0 && m < 60) ? h * 60 + m : -1;
}
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

WeatherNow WeatherService::Current() {
    std::lock_guard<std::mutex> lock(mutex_);
    return now_;
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
    TickType_t next_fetch = xTaskGetTickCount() + pdMS_TO_TICKS(kFirstDelayMs);
    while (true) {
        const TickType_t now = xTaskGetTickCount();
        const int32_t wait = int32_t(next_fetch - now);
        ulTaskNotifyTake(pdTRUE, wait > 0 ? TickType_t(wait) : 0);
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
        if (int32_t(xTaskGetTickCount() - next_fetch) >= 0) {
            const bool ok = Fetch();
            next_fetch = xTaskGetTickCount() +
                         pdMS_TO_TICKS((ok ? kRefreshSeconds : kRetrySeconds) * 1000);
        }
    }
}

bool WeatherService::Fetch() {
    auto network = Board::GetInstance().GetNetwork();
    if (!network)
        return false;
    auto http = network->CreateHttp(0);
    if (!http)
        return false;
    char url[320];
    std::snprintf(url, sizeof(url),
                  "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                  "&current=weather_code&daily=sunrise,sunset&timezone=Asia%%2FShanghai"
                  "&forecast_days=1",
                  latitude_, longitude_);
    http->SetTimeout(8000);
    http->SetHeader("Accept", "application/json");
    std::string body;
    int status = 0;
    if (http->Open("GET", url)) {
        const auto code = http->GetStatusCode();
        status = code ? *code : 0;
        char buffer[512];
        while (status == 200 && body.size() < kMaxBody) {
            auto read = http->Read(buffer, sizeof(buffer));
            if (!read || *read == 0)
                break;
            body.append(buffer, *read);
        }
        http->Close();
    }
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
        body.empty() ? nullptr : cJSON_ParseWithLength(body.data(), body.size()), cJSON_Delete);
    auto* current = root ? cJSON_GetObjectItem(root.get(), "current") : nullptr;
    auto* code = current ? cJSON_GetObjectItem(current, "weather_code") : nullptr;
    auto* daily = root ? cJSON_GetObjectItem(root.get(), "daily") : nullptr;
    auto first = [&](const char* key) -> const char* {
        auto* list = daily ? cJSON_GetObjectItem(daily, key) : nullptr;
        auto* item = cJSON_IsArray(list) ? cJSON_GetArrayItem(list, 0) : nullptr;
        return cJSON_IsString(item) ? item->valuestring : nullptr;
    };
    if (status != 200 || !cJSON_IsNumber(code)) {
        ESP_LOGW(TAG, "weather fetch failed (http %d, %u bytes)", status, unsigned(body.size()));
        return false;
    }
    WeatherNow next;
    next.valid = true;
    next.wmo_code = code->valueint;
    next.sunrise_min = LocalMinutes(first("sunrise"));
    next.sunset_min = LocalMinutes(first("sunset"));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        next.version = now_.version + 1;
        now_ = next;
    }
    ESP_LOGI(TAG, "weather code=%d sunrise=%02d:%02d sunset=%02d:%02d", next.wmo_code,
             next.sunrise_min / 60, next.sunrise_min % 60, next.sunset_min / 60,
             next.sunset_min % 60);
    return true;
}

}  // namespace tab5_sky
