#pragma once

#include <driver/ledc.h>
#include <esp_timer.h>

#include <atomic>
#include <cstdint>

#include "board.h"
#include "lvgl.h"

// Page change as a fade through dark, done with the backlight PWM instead of LVGL:
// a full 1280x720 LVGL redraw takes 50-100 ms here, so any drawn transition (slide or
// opacity) shows only a few frames. The panel dims in ~60 ms, LVGL draws the new page
// while it is dark, and once that frame has been flushed the light comes back in
// ~200 ms. The PWM steps run on an esp_timer every 5 ms and cost no drawing.
//
// Uses LEDC channel 0 / low-speed mode, as configured by PwmBacklight. Brightness is
// read from the Backlight on every step, so a concurrent user change still wins.
namespace tab5_ui {

class PageFade {
public:
    static PageFade& Get() {
        static PageFade fade;
        return fade;
    }

    // LVGL task. Call right after the page switch.
    void Start() {
        if (!timer_ && !Init())
            return;
        if (UserBrightness() == 0)
            return;  // screen off / asleep: nothing to fade
        rendered_.store(false, std::memory_order_relaxed);
        phase_start_us_ = esp_timer_get_time();
        // Restart from the current level so a quick second tap does not flash.
        from_ = level_;
        phase_.store(kDimming, std::memory_order_release);
        esp_timer_stop(timer_);
        esp_timer_start_periodic(timer_, kStepUs);
    }

private:
    enum Phase : int { kIdle, kDimming, kWaitRender, kBrightening };
    static constexpr int64_t kStepUs = 5000;
    static constexpr int64_t kDimUs = 60000;
    static constexpr int64_t kRiseUs = 200000;
    static constexpr int64_t kMaxDarkUs = 260000;  // never stay dark longer than this
    static constexpr int kDarkLevel = 18;           // of 255: a hint of the page stays visible

    PageFade() = default;

    bool Init() {
        const esp_timer_create_args_t args = {.callback = [](void* arg) { static_cast<PageFade*>(arg)->Step(); },
                                              .arg = this,
                                              .dispatch_method = ESP_TIMER_TASK,
                                              .name = "page_fade",
                                              .skip_unhandled_events = true};
        if (esp_timer_create(&args, &timer_) != ESP_OK) {
            timer_ = nullptr;
            return false;
        }
        if (auto* display = lv_display_get_default())
            lv_display_add_event_cb(display, OnRefrReady, LV_EVENT_REFR_READY, this);
        return true;
    }

    // A frame was rendered and flushed after the switch: the new page is on the panel.
    static void OnRefrReady(lv_event_t* event) {
        auto* self = static_cast<PageFade*>(lv_event_get_user_data(event));
        if (self->phase_.load(std::memory_order_acquire) != kIdle)
            self->rendered_.store(true, std::memory_order_release);
    }

    static int UserBrightness() {
        auto* backlight = Board::GetInstance().GetBacklight();
        return backlight ? backlight->brightness() : 0;
    }

    void Apply(int level) {
        level_ = level;
        // Perceptual curve: brightness feels roughly quadratic in duty.
        const uint32_t full = uint32_t(1023 * UserBrightness() / 100);
        const uint32_t duty = full * uint32_t(level * level) / (255u * 255u);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }

    void Step() {
        const int64_t now = esp_timer_get_time();
        const int64_t age = now - phase_start_us_;
        switch (phase_.load(std::memory_order_acquire)) {
            case kDimming: {
                const int64_t t = age < kDimUs ? age : kDimUs;
                Apply(from_ - int((from_ - kDarkLevel) * t / kDimUs));
                if (age >= kDimUs)
                    phase_.store(kWaitRender, std::memory_order_release);
                break;
            }
            case kWaitRender:
                if (rendered_.load(std::memory_order_acquire) || age >= kMaxDarkUs) {
                    phase_start_us_ = now;
                    phase_.store(kBrightening, std::memory_order_release);
                }
                break;
            case kBrightening: {
                if (age >= kRiseUs) {
                    Apply(255);
                    phase_.store(kIdle, std::memory_order_release);
                    esp_timer_stop(timer_);
                    break;
                }
                // Ease out: fast at first, settling gently.
                const float u = float(age) / float(kRiseUs);
                const float eased = 1.0f - (1.0f - u) * (1.0f - u);
                Apply(kDarkLevel + int((255 - kDarkLevel) * eased));
                break;
            }
            default:
                esp_timer_stop(timer_);
                break;
        }
    }

    esp_timer_handle_t timer_ = nullptr;
    std::atomic<int> phase_{kIdle};
    std::atomic<bool> rendered_{false};
    int64_t phase_start_us_ = 0;
    int from_ = 255;
    int level_ = 255;
};

}  // namespace tab5_ui
