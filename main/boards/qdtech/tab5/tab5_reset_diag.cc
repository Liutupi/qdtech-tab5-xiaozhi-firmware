#include "tab5_reset_diag.h"

#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>

#include <cstdio>

#include "esp_private/panic_internal.h"
#include "riscv/rvruntime-frames.h"

extern "C" int _iram_text_start, _iram_text_end, _text_start, _text_end;
extern "C" char* g_panic_abort_details;

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

// Where the last panic happened: registers, the abort/assert text and code addresses
// found on the stack (a heuristic backtrace). Decode with addr2line on the same ELF.
constexpr uint32_t kCrashMagic = 0x7ab5c4a5;
constexpr int kStackPcs = 16;
struct Crash {
    uint32_t magic;
    uint32_t mcause, mepc, ra, sp, mtval;
    uint32_t pcs[kStackPcs];
    char reason[40];
    char details[120];
};
RTC_NOINIT_ATTR Crash g_crash;

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

IRAM_ATTR void CopyText(char* out, size_t n, const char* in) {
    size_t i = 0;
    for (; in && i + 1 < n && in[i]; ++i)
        out[i] = in[i];
    out[i] = 0;
}

IRAM_ATTR bool IsCode(uint32_t v) {
    return (v >= uint32_t(&_iram_text_start) && v < uint32_t(&_iram_text_end)) ||
           (v >= uint32_t(&_text_start) && v < uint32_t(&_text_end));
}

void LogCrash() {
    if (g_crash.magic != kCrashMagic)
        return;
    g_crash.magic = 0;
    g_crash.reason[sizeof(g_crash.reason) - 1] = 0;
    g_crash.details[sizeof(g_crash.details) - 1] = 0;
    ESP_LOGW(TAG, "last panic: %s | %s", g_crash.reason, g_crash.details);
    ESP_LOGW(TAG, "last panic: mcause=0x%08lx mepc=0x%08lx ra=0x%08lx sp=0x%08lx mtval=0x%08lx",
             (unsigned long)g_crash.mcause, (unsigned long)g_crash.mepc, (unsigned long)g_crash.ra,
             (unsigned long)g_crash.sp, (unsigned long)g_crash.mtval);
    char line[kStackPcs * 11 + 1];
    size_t used = 0;
    for (int i = 0; i < kStackPcs && g_crash.pcs[i]; ++i)
        used += std::snprintf(line + used, sizeof(line) - used, " 0x%08lx", (unsigned long)g_crash.pcs[i]);
    line[used] = 0;
    ESP_LOGW(TAG, "last panic stack pcs:%s", line);
}

}  // namespace

void Start() {
    LogCrash();
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

extern "C" void __real_esp_panic_handler(panic_info_t* info);
extern "C" IRAM_ATTR void __wrap_esp_panic_handler(panic_info_t* info) {
    using namespace tab5_diag;
    const auto* frame = static_cast<const RvExcFrame*>(info->frame);
    g_crash.mcause = frame ? frame->mcause : 0;
    g_crash.mepc = frame ? frame->mepc : 0;
    g_crash.ra = frame ? frame->ra : 0;
    g_crash.sp = frame ? frame->sp : 0;
    g_crash.mtval = frame ? frame->mtval : 0;
    CopyText(g_crash.reason, sizeof(g_crash.reason), info->reason);
    CopyText(g_crash.details, sizeof(g_crash.details),
             // abort()/assert reach here as an illegal instruction; their text is set first.
             g_panic_abort_details ? g_panic_abort_details : info->description);
    int found = 0;
    const uint32_t sp = g_crash.sp & ~3u;
    if (sp)
        for (int i = 0; i < 256 && found < kStackPcs; ++i) {
            const uint32_t v = reinterpret_cast<const uint32_t*>(sp)[i];
            if (IsCode(v) && !(v & 1))
                g_crash.pcs[found++] = v;
        }
    for (int i = found; i < kStackPcs; ++i)
        g_crash.pcs[i] = 0;
    g_crash.magic = kCrashMagic;
    __real_esp_panic_handler(info);
}
