#pragma once

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "application.h"
#include "assets/lang_config.h"
#include "display/lcd_display.h"
#include "display/lvgl_display/lvgl_font.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "tab5_native_apps.h"
#include "font/binfont_loader/lv_binfont_loader.h"
#include "nabo_assets.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "tab5_daily_content.h"
#include "tab5_lrc.h"
#include "tab5_native_apps.h"

LV_FONT_DECLARE(qd_font_lxgw_28);
LV_FONT_DECLARE(qd_font_cjk_28);
LV_FONT_DECLARE(qd_font_lxgw_36);
LV_FONT_DECLARE(qd_font_clock_72);

// LVGL draws 1280x720 and the port rotates only dirty areas to ST7121 scanout.
class QdtechTab5Display : public MipiLcdDisplay {
    lv_obj_t* portrait_ = nullptr;
    lv_obj_t* blink_ = nullptr;
    lv_obj_t* mouth_ = nullptr;
    lv_obj_t* wave_ = nullptr;
    lv_obj_t* sleep_ = nullptr;
    lv_obj_t* ambient_[3] = {};
    lv_obj_t* sleep_z_[3] = {};
    lv_obj_t* daily_title_label_ = nullptr;
    lv_obj_t* daily_body_label_ = nullptr;
    lv_obj_t* daily_dots_[6] = {};
    // 0-2: pushed medical digest; 3-5: quote / history / festival.
    std::string daily_titles_[6];
    std::string daily_bodies_[6];
    int daily_date_key_ = -1;
    unsigned daily_page_ = 0;
    std::string digest_text_;
    bool has_digest_ = false;
    lv_font_t* sd_fallback_font_ = nullptr;
    unsigned next_blink_tick_ = 65;
    unsigned blink_step_ = 0;
    int wave_frame_ = -1;
    lv_obj_t* message_label_ = nullptr;
    lv_obj_t* prompt_label_ = nullptr;
    lv_obj_t* button_label_ = nullptr;
    std::unique_ptr<Tab5NativeApps> apps_;
    std::function<void()> radio_voice_action_;
    lv_obj_t* date_label_ = nullptr;
    lv_obj_t* clock_digits_[6] = {};
    lv_obj_t* clock_flaps_[6] = {};
    lv_obj_t* clock_flap_digits_[6] = {};
    char clock_digit_values_[6] = {'-', '-', '-', '-', '-', '-'};
    time_t last_clock_second_ = 0;
    int last_date_key_ = -1;
    lv_obj_t* waveform_[5] = {};
    lv_timer_t* animation_timer_ = nullptr;
    unsigned tick_ = 0;
    unsigned wave_ticks_ = 0;
    unsigned greeting_ticks_ = 0;
    bool active_ = false;
    bool speaking_ = false;
    bool sleeping_ = false;
    bool preview_active_ = false;
    std::function<void()> presence_test_action_;
    std::function<void()> interaction_action_;
    unsigned mouth_frame_ = 0;
    std::string music_title_;
    std::string music_artist_;
    tab5_lrc::Document music_lyrics_;
    int music_line_index_ = -2;
    int64_t music_elapsed_ms_ = 0;
    int64_t music_resume_ms_ = 0;
    bool music_playing_ = false;
    bool music_active_ = false;
    bool music_seen_station_ = false;

    void RefreshMusicLyrics() {
        if (music_lyrics_.lines.empty())
            return;
        const int64_t now_ms = esp_timer_get_time() / 1000;
        const int64_t elapsed =
            music_elapsed_ms_ + (music_playing_ ? now_ms - music_resume_ms_ : 0);
        const int index = tab5_lrc::ActiveLine(
            music_lyrics_, static_cast<uint32_t>(std::max<int64_t>(0, elapsed)));
        if (index == music_line_index_)
            return;
        music_line_index_ = index;
        const auto& lines = music_lyrics_.lines;
        const char* previous = index > 0 ? lines[index - 1].text.c_str() : "";
        const char* current = index >= 0 ? lines[index].text.c_str() : "♪";
        const char* next =
            index + 1 < static_cast<int>(lines.size()) ? lines[index + 1].text.c_str() : "";
        if (message_label_)
            lv_label_set_text(message_label_, current);
        if (apps_)
            apps_->SetMusicLyricsWindow(music_title_.c_str(), music_artist_.c_str(), previous,
                                        current, next);
    }

    static void ClockFlapScale(void* obj, int32_t value) {
        lv_obj_set_style_transform_scale_y(static_cast<lv_obj_t*>(obj), value, 0);
    }

    void ShowDailyPage(unsigned page) {
        daily_page_ = page % 6;
        if (!daily_title_label_ || !daily_body_label_)
            return;
        lv_label_set_text(daily_title_label_, daily_titles_[daily_page_].c_str());
        lv_label_set_text(daily_body_label_, daily_bodies_[daily_page_].c_str());
        for (unsigned i = 0; i < 6; ++i)
            lv_obj_set_style_bg_opa(daily_dots_[i], i == daily_page_ ? LV_OPA_COVER : LV_OPA_30, 0);
        if (has_digest_ && daily_page_ < 3) {
            digest_text_ = daily_titles_[daily_page_] + "\n" + daily_bodies_[daily_page_];
            if (!active_ && !speaking_ && !music_active_) {
                if (prompt_label_)
                    lv_label_set_text(prompt_label_, "今日医学精选");
                if (message_label_)
                    lv_label_set_text(message_label_, digest_text_.c_str());
            }
        }
    }

    // Fill builtin trio immediately so the carousel is never empty before SNTP.
    void InitDailyFallback() {
        daily_titles_[3] = "每日一句";
        daily_bodies_[3] = "正在校时…";
        daily_titles_[4] = "历史上的今天";
        daily_bodies_[4] = "正在校时…";
        daily_titles_[5] = "节日提醒";
        daily_bodies_[5] = "正在校时…";
        if (!has_digest_) {
            daily_titles_[0] = "今日医学";
            daily_bodies_[0] = "等待推送…";
            daily_titles_[1] = "今日医学";
            daily_bodies_[1] = "等待推送…";
            daily_titles_[2] = "今日医学";
            daily_bodies_[2] = "等待推送…";
        }
        ShowDailyPage(has_digest_ ? 0 : 3);
    }

    void UpdateDailyContent(const tm& date) {
        const int key = (date.tm_year + 1900) * 10000 + (date.tm_mon + 1) * 100 + date.tm_mday;
        if (key == daily_date_key_)
            return;
        if (daily_date_key_ != -1 && has_digest_) {
            has_digest_ = false;
            digest_text_.clear();
            if (!active_ && !speaking_ && !music_active_) {
                if (prompt_label_)
                    lv_label_set_text(prompt_label_, "随时倾听");
                if (message_label_)
                    lv_label_set_text(message_label_, "轻触下方按钮，开始对话。");
            }
        }
        daily_date_key_ = key;
        const auto content = Tab5DailyContentForDate(date);
        // Pages 3-5 keep the original daily trio.
        daily_titles_[3] = "每日一句";
        daily_bodies_[3] = content.quote;
        daily_titles_[4] = "历史上的今天";
        daily_bodies_[4] = content.history_text
                               ? std::string(content.history_year) + " · " + content.history_text
                               : "今天暂无收录的历史事件";
        daily_titles_[5] = "节日提醒";
        if (content.festival_title) {
            daily_bodies_[5] =
                std::string("今天是") + content.festival_title + " · " + content.festival_text;
        } else if (content.next_festival_title) {
            daily_bodies_[5] = std::string(content.next_festival_title) + "还有" +
                               std::to_string(content.days_to_next_festival) + "天";
        } else {
            daily_bodies_[5] = "愿今天也有值得记住的小事";
        }
        if (!has_digest_) {
            daily_titles_[0] = "今日医学";
            daily_bodies_[0] = "等待推送…";
            daily_titles_[1] = "今日医学";
            daily_bodies_[1] = "等待推送…";
            daily_titles_[2] = "今日医学";
            daily_bodies_[2] = "等待推送…";
        }
        ShowDailyPage(has_digest_ ? 0 : 3);
    }

    static void PresenceLongPress(lv_event_t* event) {
        auto* self = static_cast<QdtechTab5Display*>(lv_event_get_user_data(event));
        if (self->presence_test_action_)
            self->presence_test_action_();
    }

    static bool IsRadioRequest(const char* role, const char* content) {
        if (!role || std::strcmp(role, "user") != 0 || !content)
            return false;
        const std::string_view text(content);
        for (const char* negative : {"不要", "不想", "停止", "关闭", "别播", "别放"})
            if (text.find(negative) != std::string_view::npos)
                return false;
        for (const char* phrase : {"听广播", "播放广播", "放广播", "听电台", "打开电台"})
            if (text.find(phrase) != std::string_view::npos)
                return true;
        return false;
    }

    void UpdateClock() {
        const time_t now = time(nullptr);
        if (now == last_clock_second_)
            return;
        last_clock_second_ = now;
        tm local{};
        if (!localtime_r(&now, &local) || local.tm_year < 125)
            return;

        char digits[7];
        std::snprintf(digits, sizeof(digits), "%02d%02d%02d", local.tm_hour, local.tm_min,
                      local.tm_sec);
        for (int i = 0; i < 6; ++i) {
            if (clock_digit_values_[i] == digits[i])
                continue;
            const char old = clock_digit_values_[i];
            clock_digit_values_[i] = digits[i];
            char text[2] = {digits[i], 0};
            lv_label_set_text(clock_digits_[i], text);
            if (old < '0' || old > '9')
                continue;
            char previous[2] = {old, 0};
            lv_label_set_text(clock_flap_digits_[i], previous);
            lv_anim_delete(clock_flaps_[i], ClockFlapScale);
            lv_obj_set_style_transform_scale_y(clock_flaps_[i], 256, 0);
            lv_obj_remove_flag(clock_flaps_[i], LV_OBJ_FLAG_HIDDEN);
            lv_anim_t flip;
            lv_anim_init(&flip);
            lv_anim_set_var(&flip, clock_flaps_[i]);
            lv_anim_set_values(&flip, 256, 0);
            lv_anim_set_duration(&flip, 180);
            lv_anim_set_path_cb(&flip, lv_anim_path_ease_in);
            lv_anim_set_exec_cb(&flip, ClockFlapScale);
            lv_anim_set_completed_cb(&flip, [](lv_anim_t* animation) {
                auto* flap = static_cast<lv_obj_t*>(animation->var);
                lv_obj_add_flag(flap, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_transform_scale_y(flap, 256, 0);
            });
            lv_anim_start(&flip);
        }
        const int date_key = local.tm_year * 366 + local.tm_yday;
        if (date_key != last_date_key_) {
            static constexpr const char* weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
            char date[48];
            std::snprintf(date, sizeof(date), "%04d.%02d.%02d  周%s", local.tm_year + 1900,
                          local.tm_mon + 1, local.tm_mday, weekdays[local.tm_wday]);
            lv_label_set_text(date_label_, date);
            last_date_key_ = date_key;
        }
        UpdateDailyContent(local);
    }

    static lv_obj_t* Label(lv_obj_t* parent, const char* value, const lv_font_t* font,
                           uint32_t color, int x, int y, int width) {
        auto* label = lv_label_create(parent);
        lv_label_set_text(label, value);
        lv_obj_set_pos(label, x, y);
        // long_mode first: setting WRAP after width resets the object to
        // LV_SIZE_CONTENT and the line gets clipped by the parent card.
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(label, width);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
        return label;
    }

    static lv_obj_t* Card(lv_obj_t* parent, int x, int y, int w, int h, uint32_t fill,
                          uint32_t border, int radius) {
        auto* obj = lv_obj_create(parent);
        lv_obj_set_pos(obj, x, y);
        lv_obj_set_size(obj, w, h);
        lv_obj_set_style_radius(obj, radius, 0);
        lv_obj_set_style_bg_color(obj, lv_color_hex(fill), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(obj, lv_color_hex(border), 0);
        lv_obj_set_style_border_width(obj, 1, 0);
        lv_obj_set_style_pad_all(obj, 0, 0);
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        return obj;
    }

    void ShowWave() {
        if (interaction_action_)
            interaction_action_();
        if (sleeping_) {
            sleeping_ = false;
            lv_obj_add_flag(sleep_, LV_OBJ_FLAG_HIDDEN);
            for (auto* z : sleep_z_)
                if (z)
                    lv_obj_add_flag(z, LV_OBJ_FLAG_HIDDEN);
            for (auto* dot : ambient_)
                if (dot)
                    lv_obj_remove_flag(dot, LV_OBJ_FLAG_HIDDEN);
        }
        wave_ticks_ = 28;
        wave_frame_ = 0;
        lv_image_set_src(wave_, &nabo_wave_low);
        lv_obj_set_x(wave_, 106);
        lv_obj_add_flag(portrait_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(blink_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
        mouth_frame_ = 0;
        lv_obj_remove_flag(wave_, LV_OBJ_FLAG_HIDDEN);
    }

    void Tick() {
        ++tick_;
        UpdateClock();
        if (apps_)
            apps_->Tick();
        if (apps_ && apps_->IsSettingsVisible() && tick_ % 40 == 0)
            apps_->RefreshWifi();
        RefreshMusicLyrics();
        if (apps_ && apps_->IsVisible())
            return;
        if (tick_ % 160 == 0)
            ShowDailyPage(daily_page_ + 1);
        if (preview_active_)
            return;
        if (sleeping_) {
            if (tick_ % 6 == 0) {
                for (unsigned i = 0; i < 3; ++i) {
                    const unsigned phase = (tick_ / 6 + i * 4) % 16;
                    lv_obj_set_pos(sleep_z_[i], 344 + static_cast<int>(i) * 32,
                                   135 - static_cast<int>(phase) * 3);
                    lv_obj_set_style_opa(
                        sleep_z_[i],
                        static_cast<lv_opa_t>(phase < 8 ? 55 + phase * 22 : 231 - (phase - 8) * 28),
                        0);
                }
            }
            return;
        }
        if (greeting_ticks_)
            --greeting_ticks_;
        if (wave_ticks_) {
            const unsigned elapsed = 28 - wave_ticks_;
            const int frame = elapsed < 4 || elapsed >= 22    ? 0
                              : elapsed < 10 || elapsed >= 16 ? 1
                                                              : 2;
            if (frame != wave_frame_) {
                wave_frame_ = frame;
                lv_image_set_src(wave_, frame == 0   ? &nabo_wave_low
                                        : frame == 1 ? &nabo_wave
                                                     : &nabo_wave_side);
                lv_obj_set_x(wave_, frame == 2 ? 78 : 106);
            }
            if (--wave_ticks_ == 0) {
                lv_obj_add_flag(wave_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(portrait_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        // Blink redraws a 312x125 eye patch, never the full character.
        if (!wave_ticks_ && tick_ >= next_blink_tick_) {
            if (blink_step_ == 0 || blink_step_ == 2) {
                lv_image_set_src(blink_, &nabo_half_blink);
                lv_obj_remove_flag(blink_, LV_OBJ_FLAG_HIDDEN);
            } else if (blink_step_ == 1) {
                lv_image_set_src(blink_, &nabo_closed_blink);
            } else {
                lv_obj_add_flag(blink_, LV_OBJ_FLAG_HIDDEN);
            }
            blink_step_ = (blink_step_ + 1) % 4;
            next_blink_tick_ = tick_ + (blink_step_ ? 2 : 64 + (tick_ * 37) % 55);
        }
        if (tick_ % 4 == 0) {
            for (unsigned i = 0; i < 3; ++i) {
                const unsigned phase = (tick_ / 4 + i * 5) % 20;
                lv_obj_set_y(ambient_[i],
                             112 + static_cast<int>(i) * 69 - static_cast<int>(phase / 5));
                lv_obj_set_style_opa(
                    ambient_[i], static_cast<lv_opa_t>(active_ ? 120 + phase * 5 : 70 + phase * 4),
                    0);
            }
        }
        // Twelve 50 ms phoneme beats combine rest, half-open and open artwork.
        // Changes stay inside a 48x31 mouth patch.
        static constexpr unsigned kMouth[12] = {0, 1, 1, 2, 2, 1, 0, 1, 2, 1, 0, 0};
        unsigned next_mouth =
            (speaking_ || greeting_ticks_) && !wave_ticks_ ? kMouth[tick_ % 12] : 0;
        if (next_mouth != mouth_frame_) {
            mouth_frame_ = next_mouth;
            if (mouth_frame_ == 0) {
                lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_image_set_src(mouth_, mouth_frame_ == 1 ? &nabo_mouth_half : &nabo_mouth_open);
                lv_obj_remove_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        static constexpr int kIdle[5] = {12, 19, 26, 17, 11};
        static constexpr int kActive[5][5] = {
            {18, 38, 24, 43, 16}, {28, 17, 46, 22, 34}, {14, 36, 21, 42, 27},
            {32, 22, 39, 17, 30}, {20, 43, 18, 35, 22},
        };
        for (int i = 0; i < 5; ++i) {
            int height = active_ ? kActive[tick_ % 5][i] : kIdle[i];
            lv_obj_set_height(waveform_[i], height);
            lv_obj_set_y(waveform_[i], (48 - height) / 2);
        }
    }

public:
    QdtechTab5Display(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                      int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                      bool swap_xy)
        : MipiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                         swap_xy) {
        DisplayLockGuard lock(this);
        lv_display_set_rotation(display_, LV_DISPLAY_ROTATION_270);
    }

    ~QdtechTab5Display() override {
        DisplayLockGuard lock(this);
        if (animation_timer_)
            lv_timer_delete(animation_timer_);
    }

    void SetupUI() override {
        if (setup_ui_called_)
            return;
        Display::SetupUI();
        DisplayLockGuard lock(this);
        lv_image_cache_resize(3 * 1024 * 1024, false);
        auto* screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x0d1b2b), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

        Card(screen, 32, 32, 8, 53, 0x62c8e9, 0x62c8e9, 4);
        Label(screen, "NABO", &qd_font_lxgw_36, 0xf7fbff, 57, 36, 210);
        Label(screen, "土皮助手", &qd_font_lxgw_28, 0x96afc5, 193, 43, 200);

        auto* clock = lv_obj_create(screen);
        lv_obj_set_pos(clock, 405, 11);
        lv_obj_set_size(clock, 520, 90);
        lv_obj_set_style_bg_opa(clock, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(clock, 0, 0);
        lv_obj_set_style_pad_all(clock, 0, 0);
        lv_obj_clear_flag(clock, LV_OBJ_FLAG_SCROLLABLE);
        static constexpr int digit_x[6] = {0, 82, 180, 262, 360, 442};
        for (int i = 0; i < 6; ++i) {
            constexpr int width = 78;
            constexpr int height = 86;
            constexpr int half = height / 2;
            constexpr int numeral_y = 5;
            const uint32_t numeral_color = i < 2 ? 0xf8faf8 : i < 4 ? 0xf7c84d : 0x9acddb;
            auto* tile = Card(clock, digit_x[i], 2, width, height, 0x13293d, 0x31536c, 9);
            Card(tile, 1, 1, width - 2, half - 1, 0x1c3448, 0x1c3448, 8);
            clock_digits_[i] =
                Label(tile, "-", &qd_font_clock_72, numeral_color, 0, numeral_y, width);
            lv_obj_set_style_text_align(clock_digits_[i], LV_TEXT_ALIGN_CENTER, 0);
            auto* flap = Card(tile, 1, 1, width - 2, half, 0x1c3448, 0x1c3448, 8);
            lv_obj_set_style_transform_pivot_y(flap, half, 0);
            clock_flap_digits_[i] =
                Label(flap, "-", &qd_font_clock_72, numeral_color, -1, numeral_y - 1, width);
            lv_obj_set_style_text_align(clock_flap_digits_[i], LV_TEXT_ALIGN_CENTER, 0);
            clock_flaps_[i] = flap;
            lv_obj_add_flag(flap, LV_OBJ_FLAG_HIDDEN);
            Card(tile, 1, half, width - 2, 1, 0x4b6578, 0x4b6578, 0);
        }
        for (int x : {166, 346}) {
            Card(clock, x, 29, 7, 7, 0xa4d5e2, 0xa4d5e2, 4);
            Card(clock, x, 58, 7, 7, 0xa4d5e2, 0xa4d5e2, 4);
        }

        date_label_ = Label(screen, "等待校时", &qd_font_lxgw_28, 0xe4edf5, 965, 27, 280);
        lv_obj_set_style_text_align(date_label_, LV_TEXT_ALIGN_RIGHT, 0);
        status_label_ = Label(screen, "已就绪", &qd_font_lxgw_28, 0x83c9db, 970, 65, 275);
        lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_RIGHT, 0);
        notification_label_ = Label(screen, "", &qd_font_lxgw_28, 0xffd493, 884, 205, 338);
        lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

        emoji_box_ = Card(screen, 36, 112, 512, 558, 0x112842, 0x285272, 34);
        Card(emoji_box_, 42, 28, 422, 492, 0x183550, 0x183550, 220);
        portrait_ = lv_image_create(emoji_box_);
        lv_image_set_src(portrait_, &nabo_portrait);
        lv_obj_set_pos(portrait_, 39, 0);  // Original 434x618 pixels, no scaling.
        lv_obj_add_flag(portrait_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            portrait_,
            [](lv_event_t* e) {
                static_cast<QdtechTab5Display*>(lv_event_get_user_data(e))->ShowWave();
            },
            LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(portrait_, PresenceLongPress, LV_EVENT_LONG_PRESSED, this);

        blink_ = lv_image_create(emoji_box_);
        lv_image_set_src(blink_, &nabo_half_blink);
        lv_obj_set_pos(blink_, 104, 246);  // Matches source sheet crop at x65,y310.
        lv_obj_add_flag(blink_, LV_OBJ_FLAG_HIDDEN);

        mouth_ = lv_image_create(emoji_box_);
        lv_image_set_src(mouth_, &nabo_mouth_half);
        lv_obj_set_pos(mouth_, 231, 361);  // Matches portrait mouth at source x216,y441.
        lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);

        wave_ = lv_image_create(emoji_box_);
        lv_image_set_src(wave_, &nabo_wave);
        lv_obj_set_pos(wave_, 106, -4);
        lv_obj_add_flag(wave_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wave_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            wave_,
            [](lv_event_t* e) {
                static_cast<QdtechTab5Display*>(lv_event_get_user_data(e))->ShowWave();
            },
            LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(wave_, PresenceLongPress, LV_EVENT_LONG_PRESSED, this);

        sleep_ = lv_image_create(emoji_box_);
        lv_image_set_src(sleep_, &nabo_sleep);
        lv_obj_set_pos(sleep_, 15, 167);
        lv_obj_add_flag(sleep_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sleep_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(
            sleep_,
            [](lv_event_t* e) {
                static_cast<QdtechTab5Display*>(lv_event_get_user_data(e))->ShowWave();
            },
            LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(sleep_, PresenceLongPress, LV_EVENT_LONG_PRESSED, this);

        for (int i = 0; i < 3; ++i) {
            sleep_z_[i] = Label(emoji_box_, "z", &qd_font_lxgw_28, 0xa7d9ec, 344 + i * 32, 135, 28);
            lv_obj_add_flag(sleep_z_[i], LV_OBJ_FLAG_HIDDEN);
        }
        constexpr int ambient_x[3] = {67, 432, 76};
        for (int i = 0; i < 3; ++i) {
            ambient_[i] = Card(emoji_box_, ambient_x[i], 112 + i * 69, 7 + i * 2, 7 + i * 2,
                               0x81d8eb, 0x81d8eb, 12);
            lv_obj_set_style_opa(ambient_[i], LV_OPA_30, 0);
        }

        auto* daily_card = Card(screen, 581, 112, 647, 143, 0x122b43, 0x2c536c, 26);
        Card(daily_card, 24, 23, 5, 93, 0x58c9e7, 0x58c9e7, 2);
        daily_title_label_ = Label(daily_card, "每日一句", &qd_font_cjk_28, 0x76d8ed, 46, 12, 430);
        daily_body_label_ = Label(daily_card, "等待校时", &qd_font_cjk_28, 0xf5f9fd, 46, 52, 555);
        lv_obj_set_height(daily_body_label_, 82);
        for (int i = 0; i < 6; ++i)
            daily_dots_[i] = Card(daily_card, 500 + i * 16, 23, 8, 8, 0x76d8ed, 0x76d8ed, 5);
        InitDailyFallback();

        auto* card = Card(screen, 581, 264, 647, 245, 0x122b43, 0x2c536c, 28);
        Card(card, 24, 28, 5, 43, 0x58c9e7, 0x58c9e7, 2);
        prompt_label_ = Label(card, "随时倾听", &qd_font_lxgw_28, 0x76d8ed, 46, 26, 360);
        message_label_ =
            Label(card, "轻触下方按钮，开始对话。", &qd_font_cjk_28, 0xe1eff6, 46, 91, 552);
        lv_obj_set_height(message_label_, 140);

        auto* apps_button = Card(screen, 1005, 279, 209, 53, 0x193b56, 0x3a7490, 18);
        lv_obj_add_flag(apps_button, LV_OBJ_FLAG_CLICKABLE);
        Label(apps_button, "应用与设置", &qd_font_lxgw_28, 0xb6e8f4, 25, 10, 180);
        lv_obj_add_event_cb(
            apps_button,
            [](lv_event_t* event) {
                auto* self = static_cast<QdtechTab5Display*>(lv_event_get_user_data(event));
                if (self->apps_)
                    self->apps_->OpenApps();
            },
            LV_EVENT_CLICKED, this);
        auto* button = lv_btn_create(screen);
        lv_obj_set_pos(button, 581, 536);
        lv_obj_set_size(button, 647, 108);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x69d0e8), 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x99e4f1), LV_STATE_PRESSED);
        lv_obj_set_style_radius(button, 30, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_add_event_cb(
            button,
            [](lv_event_t* e) {
                auto* self = static_cast<QdtechTab5Display*>(lv_event_get_user_data(e));
                ESP_LOGI("Tab5Native", "Chat button clicked");
                self->ShowWave();
                lv_label_set_text(
                    self->status_label_,
                    self->speaking_ ? "正在倾听" : (self->active_ ? "正在结束" : "正在启动"));
                Application::GetInstance().Schedule(
                    [] { Application::GetInstance().ToggleChatState(); });
            },
            LV_EVENT_CLICKED, this);
        button_label_ = lv_label_create(button);
        lv_label_set_text(button_label_, "开始对话");
        lv_obj_set_style_text_font(button_label_, &qd_font_lxgw_36, 0);
        lv_obj_set_style_text_color(button_label_, lv_color_hex(0x0b2b3f), 0);
        lv_obj_center(button_label_);

        auto* meter = Card(screen, 1040, 655, 159, 52, 0x1b3a52, 0x1b3a52, 25);
        for (int i = 0; i < 5; ++i)
            waveform_[i] = Card(meter, 32 + i * 22, 17, 7, 18, 0x69d0e8, 0x69d0e8, 4);

        preview_image_ = lv_image_create(screen);
        lv_obj_set_size(preview_image_, 320, 240);
        lv_obj_set_pos(preview_image_, 132, 266);
        lv_image_set_scale(preview_image_, 384);  // 1.5x, fits Nabo's left card.
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

        apps_ = std::make_unique<Tab5NativeApps>(screen);

        animation_timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                static_cast<QdtechTab5Display*>(lv_timer_get_user_data(timer))->Tick();
            },
            50, this);
        UpdateClock();
    }

    void SetStatus(const char* status) override {
        DisplayLockGuard lock(this);
        if (!status_label_)
            return;
        const bool is_clock = status && std::strlen(status) == 5 && status[0] >= '0' &&
                              status[0] <= '9' && status[1] >= '0' && status[1] <= '9' &&
                              status[2] == ':' && status[3] >= '0' && status[3] <= '9' &&
                              status[4] >= '0' && status[4] <= '9';
        lv_label_set_text(status_label_, is_clock ? "已就绪" : (status ? status : ""));
        active_ = status && (std::strcmp(status, Lang::Strings::LISTENING) == 0 ||
                             std::strcmp(status, Lang::Strings::SPEAKING) == 0);
        speaking_ = status && std::strcmp(status, Lang::Strings::SPEAKING) == 0;
        if (button_label_)
            lv_label_set_text(button_label_,
                              speaking_ ? "继续对话" : (active_ ? "结束对话" : "开始对话"));
        if (prompt_label_)
            lv_label_set_text(prompt_label_, speaking_ ? "Nabo 正在回应"
                                                       : (active_ ? "Nabo 正在倾听"
                                                                  : (digest_text_.empty()
                                                                         ? "随时倾听"
                                                                         : "今日医学精选")));
        if (!active_ && !speaking_ && !digest_text_.empty() && message_label_)
            lv_label_set_text(message_label_, digest_text_.c_str());
    }

    void SetChatMessage(const char* role, const char* content) override {
        const bool play_radio = IsRadioRequest(role, content);
        const bool has = content && *content;
        // Idle digest is sticky: ignore empty/heartbeat chat frames so the
        // workbench brief stays on screen until a real conversation starts.
        if (!has && has_digest_ && !active_ && !speaking_)
            return;
        {
            DisplayLockGuard lock(this);
            if (!message_label_)
                return;
            lv_label_set_text(message_label_,
                              has ? content : "轻触下方按钮，开始对话。");
            if (role && std::strcmp(role, "assistant") == 0 && has)
                lv_label_set_text(prompt_label_, "Nabo 说");
            if (play_radio && apps_)
                apps_->OpenRadio();
        }
        if (play_radio && radio_voice_action_)
            Application::GetInstance().Schedule([action = radio_voice_action_] { action(); });
    }

    void ClearChatMessages() override {
        DisplayLockGuard lock(this);
        ShowIdlePanel();
    }

    void SetEmotion(const char* emotion) override {
        DisplayLockGuard lock(this);
        if (emotion && std::strcmp(emotion, "happy") == 0 && portrait_)
            ShowWave();
    }

    void SetSleeping(bool sleeping) {
        DisplayLockGuard lock(this);
        if (!sleep_ || sleeping_ == sleeping)
            return;
        sleeping_ = sleeping;
        wave_ticks_ = 0;
        lv_obj_add_flag(wave_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(blink_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
        for (auto* dot : ambient_)
            if (dot) {
                if (sleeping)
                    lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
                else
                    lv_obj_remove_flag(dot, LV_OBJ_FLAG_HIDDEN);
            }
        for (auto* z : sleep_z_)
            if (z) {
                if (sleeping)
                    lv_obj_remove_flag(z, LV_OBJ_FLAG_HIDDEN);
                else
                    lv_obj_add_flag(z, LV_OBJ_FLAG_HIDDEN);
            }
        if (sleeping) {
            lv_obj_add_flag(portrait_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(sleep_, LV_OBJ_FLAG_HIDDEN);
            if (prompt_label_)
                lv_label_set_text(prompt_label_, "Nabo 正在休息");
        } else {
            lv_obj_add_flag(sleep_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(portrait_, LV_OBJ_FLAG_HIDDEN);
            if (prompt_label_)
                lv_label_set_text(prompt_label_, "Nabo 醒来了");
        }
    }

    void WelcomeBack() {
        DisplayLockGuard lock(this);
        if (sleep_)
            lv_obj_add_flag(sleep_, LV_OBJ_FLAG_HIDDEN);
        sleeping_ = false;
        for (auto* z : sleep_z_)
            if (z)
                lv_obj_add_flag(z, LV_OBJ_FLAG_HIDDEN);
        for (auto* dot : ambient_)
            if (dot)
                lv_obj_remove_flag(dot, LV_OBJ_FLAG_HIDDEN);
        // Keep the pushed digest visible; only flash the greeting in the prompt.
        if (prompt_label_)
            lv_label_set_text(prompt_label_, "你回来了");
        if (message_label_) {
            if (digest_text_.empty())
                lv_label_set_text(message_label_, "见到你真开心。今天想聊些什么？");
            else
                lv_label_set_text(message_label_, digest_text_.c_str());
        }
        ShowWave();
        greeting_ticks_ = 60;
    }

    void SetPresenceTestAction(std::function<void()> action) {
        DisplayLockGuard lock(this);
        presence_test_action_ = std::move(action);
    }

    void SetAppsActions(Tab5NativeApps::Actions actions, std::function<void()> voice_play) {
        DisplayLockGuard lock(this);
        if (apps_)
            apps_->SetActions(std::move(actions));
        radio_voice_action_ = std::move(voice_play);
    }

    void ShowRadioPage() {
        DisplayLockGuard lock(this);
        if (apps_)
            apps_->OpenRadio();
    }

    void SetFirmwareStatus(const std::string& text, const std::string& button, int progress,
                           bool busy) {
        DisplayLockGuard lock(this);
        if (apps_)
            apps_->SetFirmwareStatus(text.c_str(), button.c_str(), progress, busy);
    }

    // Load a prebuilt LVGL .bin CJK font from SD and use it as fallback for
    // the flash-subset LXGW fonts (missing glyphs render blank otherwise).
    // Build: lv_font_conv --format bin --size 28 --bpp 4 --font <ttf> --symbols <chars> -o cjk28.bin
    bool LoadSdFallbackFont() {
        if (sd_fallback_font_)
            return true;
        static const char* kCandidates[] = {
            "S:/fonts/cjk28.bin",
            "/sdcard/fonts/cjk28.bin",
        };
        for (const char* cand : kCandidates) {
            FILE* f = fopen(cand, "rb");
            if (!f)
                continue;
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fclose(f);
            // Reject empty / absurdly large files before handing to LVGL.
            if (sz < 1024 || sz > 2 * 1024 * 1024) {
                ESP_LOGW("Tab5Font", "Skip %s size=%ld", cand, sz);
                continue;
            }
            auto* font = lv_binfont_create(cand);
            if (!font) {
                ESP_LOGW("Tab5Font", "lv_binfont_create failed for %s", cand);
                continue;
            }
            sd_fallback_font_ = font;
            DisplayLockGuard lock(this);
            const_cast<lv_font_t*>(&qd_font_lxgw_28)->fallback = sd_fallback_font_;
            const_cast<lv_font_t*>(&qd_font_lxgw_36)->fallback = sd_fallback_font_;
            ESP_LOGI("Tab5Font", "SD fallback font ready: %s", cand);
            return true;
        }
        ESP_LOGW("Tab5Font", "No usable /sdcard/fonts/cjk28.bin; missing glyphs stay blank");
        return false;
    }

    void ShowIcuPage(int mode = -1, const std::string& result = "") {
        DisplayLockGuard lock(this);
        if (apps_)
            apps_->OpenIcu(mode, result);
    }

    esp_lcd_panel_handle_t lcd_panel() const { return panel_; }
    lv_display_t* lv_disp() const { return display_; }

    void ShowNesPage() {
        DisplayLockGuard lock(this);
        if (apps_)
            apps_->OpenNes();
    }

    void PushNesFrame(const uint16_t* pixels, uint16_t width, uint16_t height) {
        // Scale outside the LVGL lock; only the invalidate needs the lock.
        // Holding the lock through a 1280×720 scale starves the display task.
        if (apps_) {
            apps_->SetNesFrame(pixels, width, height);
        }
    }

    void SetNesStatus(const char* status) {
        DisplayLockGuard lock(this);
        if (apps_)
            apps_->SetNesStatus(status);
    }

    void SetNesPlaying(bool playing) {
        DisplayLockGuard lock(this);
        if (apps_)
            apps_->SetNesPlaying(playing);
    }

    void SetRadioState(const char* station, const char* state, const char* meta) {
        DisplayLockGuard lock(this);
        if (!lock.locked())
            return;
        if (apps_)
            apps_->SetRadioState(station, state, meta);
    }

    void SetInteractionAction(std::function<void()> action) {
        DisplayLockGuard lock(this);
        interaction_action_ = std::move(action);
    }

    void SetVisionMessage(const char* title, const char* message) {
        DisplayLockGuard lock(this);
        if (!lock.locked())
            return;
        if (prompt_label_)
            lv_label_set_text(prompt_label_, title);
        if (message_label_)
            lv_label_set_text(message_label_, message);
    }

    // Idle panel text: pushed digest when present, otherwise the default hint.
    void ShowIdlePanel() {
        if (prompt_label_)
            lv_label_set_text(prompt_label_, digest_text_.empty() ? "随时倾听" : "今日医学精选");
        if (message_label_)
            lv_label_set_text(message_label_, digest_text_.empty()
                                                  ? "轻触下方按钮，开始对话。"
                                                  : digest_text_.c_str());
    }

    // Push external digest onto the small daily card (stable all day) and
    // mirror it in the conversation panel when idle.
    void SetDailyCards(const char* const titles[3], const char* const bodies[3]) {
        DisplayLockGuard lock(this);
        if (!lock.locked())
            return;
        for (int i = 0; i < 3; ++i) {
            const char* title = titles[i];
            const char* body = bodies[i];
            if (!title || !*title)
                continue;
            daily_titles_[i] = title;
            daily_bodies_[i] = body ? body : "";
        }
        has_digest_ = true;
        ShowDailyPage(0);
    }

    void RestoreDigestIfIdle() {
        if (digest_text_.empty() || active_ || speaking_)
            return;
        if (prompt_label_)
            lv_label_set_text(prompt_label_, "今日医学精选");
        if (message_label_)
            lv_label_set_text(message_label_, digest_text_.c_str());
    }

    void SetMusicInfo(const char* title, const char* artist, const char* line) {
        DisplayLockGuard lock(this);
        if (!lock.locked())
            return;
        if (auto* theme = LvglThemeManager::GetInstance().GetTheme("dark")) {
            auto font = theme->GetTextFont();
            if (font && font->font()) {
                if (prompt_label_)
                    lv_obj_set_style_text_font(prompt_label_, font->font(), 0);
                if (message_label_)
                    lv_obj_set_style_text_font(message_label_, font->font(), 0);
            }
        }
        if (title && *title) {
            if (music_title_ != title)
                music_artist_ = artist ? artist : "";
            music_title_ = title;
        }
        if (artist && *artist)
            music_artist_ = artist;
        char head[96];
        std::snprintf(head, sizeof(head), "%s%s%s", music_title_.c_str(),
                      music_artist_.empty() ? "" : " · ", music_artist_.c_str());
        if (prompt_label_)
            lv_label_set_text(prompt_label_, head);
        if (message_label_)
            lv_label_set_text(message_label_, (line && *line) ? line : "正在播放");
        if (button_label_)
            lv_label_set_text(button_label_, "继续对话");
        // Songs play through the radio page — keep lyric strip there in sync.
        if (apps_) {
            apps_->SetMusicLyric(music_title_.c_str(), music_artist_.c_str(), line);
            apps_->OpenRadio();
        }
    }

    void BeginMusicTrack(const char* title, const char* artist) {
        DisplayLockGuard lock(this);
        if (!lock.locked())
            return;
        music_title_ = title && *title ? title : "音乐";
        music_artist_ = artist ? artist : "";
        music_lyrics_ = {};
        music_line_index_ = -2;
        music_elapsed_ms_ = 0;
        music_resume_ms_ = 0;
        music_playing_ = false;
        music_active_ = true;
        music_seen_station_ = false;
        if (apps_)
            apps_->ClearMusicLyrics();
    }

    void UpdateMusicPlaybackState(const char* station, const char* state) {
        DisplayLockGuard lock(this);
        if (!lock.locked() || !music_active_)
            return;
        const std::string expected =
            music_title_ + (music_artist_.empty() ? "" : " - " + music_artist_);
        if (station && *station && station == expected)
            music_seen_station_ = true;
        if (music_seen_station_ && station && *station && station != expected) {
            music_active_ = false;
            music_playing_ = false;
            music_lyrics_ = {};
            if (apps_)
                apps_->ClearMusicLyrics();
            return;
        }
        if (!music_seen_station_)
            return;
        const bool playing = state && std::strcmp(state, "Playing") == 0;
        if (playing == music_playing_)
            return;
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (playing)
            music_resume_ms_ = now_ms;
        else
            music_elapsed_ms_ += now_ms - music_resume_ms_;
        music_playing_ = playing;
    }

    bool SetMusicLyrics(const char* lrc, const char* title = nullptr) {
        if (!lrc)
            return false;
        tab5_lrc::Document parsed;
        if (!tab5_lrc::Parse(lrc, parsed))
            return false;
        DisplayLockGuard lock(this);
        if (!lock.locked() || !music_active_ || (title && *title && music_title_ != title))
            return false;
        music_lyrics_ = std::move(parsed);
        music_line_index_ = -2;
        ESP_LOGI("Tab5Native", "music LRC lines=%u timed=%d", unsigned(music_lyrics_.lines.size()),
                 music_lyrics_.timed);
        RefreshMusicLyrics();
        return true;
    }

    void StopMusicTrack() {
        DisplayLockGuard lock(this);
        if (!lock.locked())
            return;
        music_active_ = false;
        music_playing_ = false;
        music_seen_station_ = false;
        music_lyrics_ = {};
        if (apps_)
            apps_->ClearMusicLyrics();
    }

    void SetVisionPreview(const uint8_t* rgb) {
        DisplayLockGuard lock(this);
        if (!preview_image_)
            return;
        if (!rgb) {
            preview_active_ = false;
            lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
            lv_image_set_src(preview_image_, nullptr);
            if (preview_image_cached_)
                lv_image_cache_drop(preview_image_cached_->image_dsc());
            preview_image_cached_.reset();
            if (!sleeping_)
                lv_obj_remove_flag(portrait_, LV_OBJ_FLAG_HIDDEN);
            for (auto* dot : ambient_)
                if (dot && !sleeping_)
                    lv_obj_remove_flag(dot, LV_OBJ_FLAG_HIDDEN);
            for (auto* z : sleep_z_)
                if (z && sleeping_)
                    lv_obj_remove_flag(z, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        constexpr size_t kPixels = 320 * 240;
        auto* pixels = static_cast<uint16_t*>(
            heap_caps_malloc(kPixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!pixels)
            return;
        for (size_t i = 0; i < kPixels; ++i) {
            const uint8_t* p = rgb + i * 3;
            pixels[i] = uint16_t(((p[0] & 0xf8) << 8) | ((p[1] & 0xfc) << 3) | (p[2] >> 3));
        }
        lv_image_set_src(preview_image_, nullptr);
        if (preview_image_cached_)
            lv_image_cache_drop(preview_image_cached_->image_dsc());
        preview_image_cached_ = std::make_unique<LvglAllocatedImage>(
            pixels, kPixels * sizeof(uint16_t), 320, 240, 320 * 2, LV_COLOR_FORMAT_RGB565);
        lv_image_set_src(preview_image_, preview_image_cached_->image_dsc());
        preview_active_ = true;
        lv_obj_add_flag(portrait_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(blink_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wave_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sleep_, LV_OBJ_FLAG_HIDDEN);
        for (auto* dot : ambient_)
            if (dot)
                lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
        for (auto* z : sleep_z_)
            if (z)
                lv_obj_add_flag(z, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    }

    void SetTheme(Theme* theme) override { Display::SetTheme(theme); }
};
