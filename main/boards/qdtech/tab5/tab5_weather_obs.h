#pragma once

#include <cJSON.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Weather from China Meteorological Administration station observations
// (www.nmc.cn/rest/real/<station>), plus local sunrise/sunset. Header-only and free of
// ESP/LVGL so it is host-tested.
//
// Why: Open-Meteo's default model forecast disagreed with the actual weather (e.g. drizzle
// under a clear sky in Zhongshan), while the CMA station reports what is observed.
namespace tab5_weather {

// Zhongshan, Guangdong (www.nmc.cn/rest/province/AGD).
constexpr const char* kDefaultCmaStation = "YIWau";

struct Observation {
    bool valid = false;
    double temperature = 0;  // °C
    int humidity = -1;       // %
    double rain = 0;         // mm in the last hour
    int wmo_code = -1;       // WMO-style code the UI already understands
    char info[24] = {};      // CMA wording, e.g. 晴 / 多云 / 小雨
    char time[6] = {};       // "HH:MM" of the observation
    double feels_like = 0;   // °C (CMA "feelst"); equals temperature when not reported
    char wind[32] = {};      // e.g. 东南风 微风
    bool warning = false;    // an active CMA weather warning (typhoon, rainstorm...)
    int warning_level = 0;   // 0 none, 1 blue/white, 2 yellow, 3 orange, 4 red
    char warning_title[48] = {};  // short, e.g. 暴雨黄色预警
    char warning_text[160] = {};  // the full alert line, e.g. 中山市气象台发布暴雨黄色预警信号
};

// Copies at most cap-1 bytes without cutting a UTF-8 sequence in half.
inline void CopyUtf8(char* dst, size_t cap, const char* src) {
    if (!cap)
        return;
    size_t n = src ? std::strlen(src) : 0;
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80)
            --n;  // src[n] would be a continuation byte: drop the partial character
    }
    if (n)
        std::memcpy(dst, src, n);
    dst[n] = 0;
}

// Colour grade named in a CMA signal level or alert line (blue < yellow < orange < red).
inline int WarningLevel(const char* text) {
    if (!text)
        return 0;
    if (std::strstr(text, "红色"))
        return 4;
    if (std::strstr(text, "橙色"))
        return 3;
    if (std::strstr(text, "黄色"))
        return 2;
    if (std::strstr(text, "蓝色") || std::strstr(text, "白色"))
        return 1;
    return 0;
}

// CMA weather phenomenon codes (the "img" field) to WMO weather codes.
inline int CmaToWmo(int img) {
    switch (img) {
        case 0: return 0;     // 晴
        case 1: return 2;     // 多云
        case 2: return 3;     // 阴
        case 3: return 80;    // 阵雨
        case 4: case 5: return 95;  // 雷阵雨(伴冰雹)
        case 6: return 67;    // 雨夹雪
        case 7: case 21: return 61;   // 小雨
        case 8: case 22: return 63;   // 中雨
        case 9: case 10: case 11: case 12: case 23: case 24: case 25: return 65;  // 大雨及以上
        case 13: return 85;   // 阵雪
        case 14: case 26: return 71;  // 小雪
        case 15: case 27: return 73;  // 中雪
        case 16: case 17: case 28: return 75;  // 大雪/暴雪
        case 18: case 32: case 49: case 57: case 58: return 45;  // 雾
        case 19: return 67;   // 冻雨
        case 20: case 29: case 30: case 31: case 53: case 54: case 55: case 56: return 45;  // 沙尘/霾
        default: return -1;
    }
}

inline double JsonNum(const cJSON* obj, const char* key, double fallback) {
    const cJSON* item = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : nullptr;
    if (cJSON_IsNumber(item))
        return item->valuedouble;
    if (cJSON_IsString(item) && item->valuestring && *item->valuestring) {
        char* end = nullptr;
        const double v = std::strtod(item->valuestring, &end);
        if (end && *end == 0)
            return v;
    }
    return fallback;
}

inline const char* JsonStr(const cJSON* obj, const char* key) {
    const cJSON* item = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : nullptr;
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

// CMA fills absent text fields with "9999".
inline const char* JsonText(const cJSON* obj, const char* key) {
    const char* text = JsonStr(obj, key);
    return std::strcmp(text, "9999") == 0 ? "" : text;
}

// Parses the /rest/real/<station> object. CMA uses 9999 for "no data".
inline bool ParseCmaReal(const char* json, Observation* out) {
    *out = Observation{};
    cJSON* root = cJSON_Parse(json);
    if (!root)
        return false;
    const cJSON* weather = cJSON_GetObjectItemCaseSensitive(root, "weather");
    const double temperature = JsonNum(weather, "temperature", 9999);
    const int img = int(JsonNum(weather, "img", 9999));
    const int wmo = CmaToWmo(img);
    if (temperature > 60 || temperature < -60 || wmo < 0) {
        cJSON_Delete(root);
        return false;
    }
    out->temperature = temperature;
    const double humidity = JsonNum(weather, "humidity", 9999);
    out->humidity = humidity >= 0 && humidity <= 100 ? int(std::lround(humidity)) : -1;
    const double rain = JsonNum(weather, "rain", 9999);
    out->rain = rain >= 0 && rain < 1000 ? rain : 0;
    out->wmo_code = wmo;
    const double feels = JsonNum(weather, "feelst", 9999);
    out->feels_like = feels > -60 && feels < 70 ? feels : temperature;
    CopyUtf8(out->info, sizeof(out->info), JsonText(weather, "info"));
    const cJSON* wind = cJSON_GetObjectItemCaseSensitive(root, "wind");
    const char* direct = JsonText(wind, "direct");
    const char* power = JsonText(wind, "power");
    char wind_text[64];
    std::snprintf(wind_text, sizeof(wind_text), "%s%s%s", direct, *direct && *power ? " " : "", power);
    CopyUtf8(out->wind, sizeof(out->wind), wind_text);
    // "2026-10-12 15:20" -> "15:20"
    const char* published = JsonStr(root, "publish_time");
    const char* space = std::strchr(published, ' ');
    if (space && std::strlen(space + 1) >= 5)
        std::snprintf(out->time, sizeof(out->time), "%.5s", space + 1);
    const cJSON* warn = cJSON_GetObjectItemCaseSensitive(root, "warn");
    const char* alert = JsonText(warn, "alert");
    const char* type = JsonText(warn, "signaltype");
    const char* level = JsonText(warn, "signallevel");
    if (*alert || *type) {
        out->warning = true;
        CopyUtf8(out->warning_text, sizeof(out->warning_text), *alert ? alert : type);
        out->warning_level = WarningLevel(*level ? level : alert);
        char title[96];
        if (*type && *level) {
            std::snprintf(title, sizeof(title), "%s%s预警", type, level);
        } else {
            // "中山市气象台发布暴雨黄色预警信号" -> "暴雨黄色预警"
            const char* from = std::strstr(alert, "发布");
            std::snprintf(title, sizeof(title), "%s", from ? from + std::strlen("发布") : alert);
            if (char* signal = std::strstr(title, "信号"))
                *signal = 0;
        }
        CopyUtf8(out->warning_title, sizeof(out->warning_title), *title ? title : "天气预警");
    }
    out->valid = true;
    cJSON_Delete(root);
    return true;
}

struct SunTimes {
    int sunrise_min = -1;  // local minutes after midnight
    int sunset_min = -1;
};

// NOAA solar position approximation (good to about a minute at these latitudes).
// tz_hours: offset of local time from UTC (China: 8).
inline SunTimes ComputeSun(double latitude, double longitude, int year, int month, int day, double tz_hours) {
    constexpr double kPi = 3.14159265358979323846;
    auto rad = [&](double d) { return d * kPi / 180.0; };
    // Day of year.
    static const int kCum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    const int doy = kCum[(month - 1) % 12] + day + (leap && month > 2 ? 1 : 0);
    const double gamma = 2 * kPi / (leap ? 366 : 365) * (doy - 1);
    const double eqtime = 229.18 * (0.000075 + 0.001868 * std::cos(gamma) - 0.032077 * std::sin(gamma) -
                                    0.014615 * std::cos(2 * gamma) - 0.040849 * std::sin(2 * gamma));
    const double decl = 0.006918 - 0.399912 * std::cos(gamma) + 0.070257 * std::sin(gamma) -
                        0.006758 * std::cos(2 * gamma) + 0.000907 * std::sin(2 * gamma) -
                        0.002697 * std::cos(3 * gamma) + 0.00148 * std::sin(3 * gamma);
    const double cos_ha = std::cos(rad(90.833)) / (std::cos(rad(latitude)) * std::cos(decl)) -
                          std::tan(rad(latitude)) * std::tan(decl);
    SunTimes out;
    if (cos_ha < -1 || cos_ha > 1)
        return out;  // polar day or night
    const double ha = std::acos(cos_ha) * 180.0 / kPi;
    const double noon = 720 - 4 * longitude - eqtime + tz_hours * 60;
    out.sunrise_min = int(std::lround(noon - 4 * ha));
    out.sunset_min = int(std::lround(noon + 4 * ha));
    return out;
}

}  // namespace tab5_weather
