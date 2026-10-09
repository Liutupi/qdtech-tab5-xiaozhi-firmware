#include "tab5_reset_diag.h"

#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>

#include <cstdio>

#include "application.h"
#include "settings.h"

namespace tab5_diag {
namespace {

const char* TAG = "Tab5Diag";
constexpr uint32_t kMagic = 0x7ab5d1a6;
constexpr int kTickMs = 2000;

struct Record {
    uint32_t magic;
    uint32_t uptime_s;
    uint32_t internal_free;
    uint32_t internal_min;
    uint32_t audio;
};
RTC_NOINIT_ATTR Record g_record;
std::string g_last;

const char* ReasonName(esp_reset_reason_t why) {
    switch (why) {
        case ESP_RST_PANIC:
            return "PANIC";
        case ESP_RST_INT_WDT:
            return "INT_WDT";
        case ESP_RST_TASK_WDT:
            return "TASK_WDT";
        case ESP_RST_WDT:
            return "WDT";
        case ESP_RST_BROWNOUT:
            return "BROWNOUT";
        default:
            return nullptr;  // power-on, USB, software, deep sleep: not a fault
    }
}

void Tick(void*) {
    g_record.uptime_s = uint32_t(esp_timer_get_time() / 1000000);
    g_record.internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    g_record.internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    g_record.audio = Application::GetInstance().IsExternalAudioActive() ? 1 : 0;
    g_record.magic = kMagic;
}

}  // namespace

void Start() {
    const esp_reset_reason_t why = esp_reset_reason();
    const char* fault = ReasonName(why);
    Settings settings("tab5diag", true);
    if (fault) {
        char text[160];
        if (g_record.magic == kMagic)
            std::snprintf(text, sizeof(text), "%s at %u s (audio %s, internal free %u KB, min %u KB)", fault,
                          unsigned(g_record.uptime_s), g_record.audio ? "on" : "off",
                          unsigned(g_record.internal_free / 1024), unsigned(g_record.internal_min / 1024));
        else
            std::snprintf(text, sizeof(text), "%s (no record from the previous run)", fault);
        settings.SetString("last", text);
        settings.SetInt("count", settings.GetInt("count", 0) + 1);
    }
    const std::string last = settings.GetString("last", "");
    const int count = settings.GetInt("count", 0);
    if (!last.empty())
        g_last = last + " · " + std::to_string(count) + " time" + (count == 1 ? "" : "s") + " since diagnostics began";
    ESP_LOGW(TAG, "this boot: reset_reason=%d%s; last abnormal reset: %s", int(why), fault ? " (FAULT)" : "",
             g_last.empty() ? "none recorded" : g_last.c_str());

    g_record.magic = 0;  // a fresh record starts with this run
    static esp_timer_handle_t timer = nullptr;
    const esp_timer_create_args_t args = {.callback = Tick, .arg = nullptr, .dispatch_method = ESP_TIMER_TASK,
                                          .name = "tab5_diag", .skip_unhandled_events = true};
    if (!timer && esp_timer_create(&args, &timer) == ESP_OK)
        esp_timer_start_periodic(timer, uint64_t(kTickMs) * 1000);
}

std::string LastAbnormalReset() { return g_last; }

}  // namespace tab5_diag
