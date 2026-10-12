#include "tab5_sky_weather.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>

#include "board.h"
#include "settings.h"
#include "tab5_net_gate.h"
#include "tab5_podcast_model.h"
#include "tab5_weather_obs.h"

namespace tab5_sky {
namespace {
const char* TAG = "NaboSky";
constexpr int kRefreshSeconds = 20 * 60;  // CMA stations report every 10-60 minutes
constexpr int kRetrySeconds = 5 * 60;
constexpr int kFirstDelayMs = 30000;  // let Wi-Fi, SNTP and the XiaoZhi session settle
constexpr size_t kMaxBody = 8 * 1024;
// The board runs on China time (SNTP sets CST-8). A fixed offset keeps sunrise/sunset
// consistent with localtime() even in the seconds before the TZ is applied.
constexpr double kUtcOffsetHours = 8.0;

// GET into a PSRAM string; returns the HTTP status (0 when unreachable).
int HttpGet(const char* url, tab5_podcast::Str* body) {
    std::lock_guard<std::mutex> gate(tab5_net::TransferGate());
    auto network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(0) : nullptr;
    if (!http)
        return 0;
    http->SetTimeout(8000);
    http->SetHeader("Accept", "application/json");
    int status = 0;
    if (http->Open("GET", url)) {
        const auto code = http->GetStatusCode();
        status = code ? *code : 0;
        char buffer[512];
        while (status == 200 && body->size() < kMaxBody) {
            auto read = http->Read(buffer, sizeof(buffer));
            if (!read || *read <= 0)
                break;
            body->append(buffer, *read);
        }
        http->Close();
    }
    return status;
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
        // The CMA station must match the location: an explicit "cma" setting, else the
        // Zhongshan station for Zhongshan coordinates, else Open-Meteo only.
        station_ = settings.GetString("cma", "");
        if (station_.empty() && std::fabs(latitude_ - 22.5176) < 0.3 && std::fabs(longitude_ - 113.3928) < 0.3)
            station_ = tab5_weather::kDefaultCmaStation;
    }
    if (xTaskCreatePinnedToCoreWithCaps(TaskEntry, "nabo_sky", 6144, this, 1, &task_, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        task_ = nullptr;
        ESP_LOGE(TAG, "weather task create failed");
    }
}

// Sunrise/sunset are computed for today's date, so they are right from the first boot
// minute without any network request.
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
        const auto sun = tab5_weather::ComputeSun(lat, lon, local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                                                  kUtcOffsetHours);
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
        const int32_t wait = int32_t(next_fetch - xTaskGetTickCount());
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
            next_fetch = xTaskGetTickCount() + pdMS_TO_TICKS((ok ? kRefreshSeconds : kRetrySeconds) * 1000);
        }
    }
}

// Observed weather (CMA station) first; the ECMWF model forecast only as a fallback.
// Open-Meteo's default "best_match" model reported drizzle under a clear sky.
bool WeatherService::Fetch() {
    int code = -1;
    const char* source = "";
    if (!station_.empty()) {
        char url[96];
        std::snprintf(url, sizeof(url), "http://www.nmc.cn/rest/real/%s", station_.c_str());
        tab5_podcast::Str body;
        const int status = HttpGet(url, &body);
        tab5_weather::Observation obs;
        if (status == 200 && tab5_weather::ParseCmaReal(body.c_str(), &obs)) {
            code = obs.wmo_code;
            source = "CMA";
            ESP_LOGI(TAG, "weather CMA %s: %s %.1fC %d%% rain %.1fmm (code %d)%s%s", obs.time, obs.info,
                     obs.temperature, obs.humidity, obs.rain, code, obs.warning ? " WARNING: " : "",
                     obs.warning ? obs.warning_text : "");
        } else {
            ESP_LOGW(TAG, "CMA weather unavailable (http %d); falling back to ECMWF", status);
        }
    }
    if (code < 0) {
        char url[320];
        std::snprintf(url, sizeof(url),
                      "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                      "&current=weather_code&models=ecmwf_ifs025&timezone=Asia%%2FShanghai&forecast_days=1",
                      latitude_, longitude_);
        tab5_podcast::Str body;
        const int status = HttpGet(url, &body);
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
            status == 200 ? cJSON_ParseWithLength(body.data(), body.size()) : nullptr, cJSON_Delete);
        auto* current = root ? cJSON_GetObjectItem(root.get(), "current") : nullptr;
        auto* item = current ? cJSON_GetObjectItem(current, "weather_code") : nullptr;
        if (!cJSON_IsNumber(item)) {
            ESP_LOGW(TAG, "weather fetch failed (http %d)", status);
            return false;
        }
        code = item->valueint;
        source = "ECMWF";
    }
    Publish(code);
    const auto now = Current();
    ESP_LOGI(TAG, "weather code=%d (%s) sunrise=%02d:%02d sunset=%02d:%02d", code, source, now.sunrise_min / 60,
             now.sunrise_min % 60, now.sunset_min / 60, now.sunset_min % 60);
    return true;
}

}  // namespace tab5_sky
