#include "tab5_native_apps.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "application.h"
#include "display/lvgl_display/lvgl_font.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "usb_gamepad_host.h"
#include "tab5_nes_video.h"
#include "misc/cache/instance/lv_image_cache.h"

LV_FONT_DECLARE(qd_font_lxgw_28);
LV_FONT_DECLARE(qd_font_lxgw_36);
LV_FONT_DECLARE(qd_font_cjk_28);
LV_FONT_DECLARE(font_noto_sans_basic_30_4);

namespace {
const lv_font_t* MusicTextFont() {
    auto* theme = LvglThemeManager::GetInstance().GetTheme("dark");
    if (theme) {
        auto font = theme->GetTextFont();
        if (font && font->font())
            return font->font();
    }
    return &font_noto_sans_basic_30_4;
}
}  // namespace

lv_obj_t* Tab5NativeApps::Card(lv_obj_t* parent, int x, int y, int width, int height, uint32_t fill,
                               uint32_t border, int radius) {
    auto* object = lv_obj_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(object, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(object, 1, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

lv_obj_t* Tab5NativeApps::Label(lv_obj_t* parent, const char* text, const lv_font_t* font,
                                uint32_t color, int x, int y, int width) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

lv_obj_t* Tab5NativeApps::Button(lv_obj_t* parent, const char* text, int x, int y,
                                 int width, int height, void (*callback)(lv_event_t*),
                                 void* user_data) {
    auto* button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x254c66), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x39718b), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, lv_color_hex(0x3f7590), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_radius(button, 18, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    auto* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &qd_font_lxgw_28, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xf1f8fc), 0);
    lv_obj_center(label);
    return button;
}

void Tab5NativeApps::Schedule(std::function<void()> action) {
    if (action) Application::GetInstance().Schedule(std::move(action));
}

Tab5NativeApps::Tab5NativeApps(lv_obj_t* screen) {
    root_ = Card(screen, 0, 0, 1280, 720, 0x0d1b2b, 0x0d1b2b, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    home_page_ = Card(root_, 0, 0, 1280, 720, 0x0d1b2b, 0x0d1b2b, 0);
    settings_page_ = Card(root_, 0, 0, 1280, 720, 0x0d1b2b, 0x0d1b2b, 0);
    radio_page_ = Card(root_, 0, 0, 1280, 720, 0x0d1b2b, 0x0d1b2b, 0);
    game_page_ = Card(root_, 0, 0, 1280, 720, 0x0d1b2b, 0x0d1b2b, 0);
    for (auto* page : {home_page_, settings_page_, radio_page_, game_page_})
        lv_obj_set_style_border_width(page, 0, 0);
    BuildHome();
    BuildSettings();
    BuildRadio();
    BuildGame();
    lv_obj_add_flag(settings_page_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(radio_page_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(game_page_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
}

void Tab5NativeApps::SetActions(Actions actions) {
    actions_ = std::move(actions);
}

void Tab5NativeApps::Show(lv_obj_t* page) {
    for (auto* candidate : {home_page_, settings_page_, radio_page_, game_page_,
                           icu_page_ ? icu_page_->object() : nullptr}) {
        if (!candidate) continue;
        lv_obj_add_flag(candidate, LV_OBJ_FLAG_HIDDEN);
    }
    if (ir_page_) ir_page_->Hide();
    if (page) {
        lv_obj_remove_flag(page, LV_OBJ_FLAG_HIDDEN);
        // Keep the panel clickable so taps don't fall through to the
        // workbench chat button underneath (that caused flash + dead UI).
        lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
}

void Tab5NativeApps::OpenApps() { Show(home_page_); }

void Tab5NativeApps::OpenSettings() {
    Show(settings_page_);
    RefreshSettings();
}

void Tab5NativeApps::OpenRadio() {
    Show(radio_page_);
    RefreshStations();
    Schedule(actions_.start_radio);
}

void Tab5NativeApps::OpenNes() {
    Show(game_page_);
    ShowGameSelect();
    Schedule(actions_.start_nes);
    Schedule(actions_.nes_status);
}

void Tab5NativeApps::OpenIr() {
    if (!ir_page_)
        ir_page_ = std::make_unique<Tab5IrRemotePage>(root_, [this] { OpenApps(); });
    for (auto* candidate : {home_page_, settings_page_, radio_page_, game_page_,
                           icu_page_ ? icu_page_->object() : nullptr}) {
        if (!candidate) continue;
        lv_obj_add_flag(candidate, LV_OBJ_FLAG_HIDDEN);
    }
    ir_page_->Show();
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
}

void Tab5NativeApps::Tick() {
    UpdateWave();
    // Gamepad-only UI: poll the pad on the game page.
    if (IsVisible() && game_page_ && !lv_obj_has_flag(game_page_, LV_OBJ_FLAG_HIDDEN)) {
        static uint8_t last_pad = 0;
        const uint8_t pad = UsbGamepadNesMask();
        if (game_pad_resync_) {
            // Back from a game (LVGL was paused): buttons still held from the
            // exit combo must not count as fresh presses (Start would restart
            // the ROM, Select would leave the page).
            game_pad_resync_ = false;
            last_pad = pad;
        }
        const uint8_t pressed = static_cast<uint8_t>(pad & ~last_pad);
        last_pad = pad;
        const bool playing = game_playing_ui_.load();
        if (!playing) {
            // ROM list mode — D-pad selects, A starts.
            if (pressed & kNesBtnUp) Schedule(actions_.nes_previous);
            if (pressed & kNesBtnDown) Schedule(actions_.nes_next);
            if (pressed & (kNesBtnA | kNesBtnStart)) {
                Schedule(actions_.nes_play_pause);
                ShowGamePlay();
            }
            // Tick runs with the display lock — safe to repaint the list.
            RefreshGameRoms();
        }
        constexpr uint8_t kExitCombo = kNesBtnSelect | kNesBtnStart;
        const bool exit_pressed = playing ? ((pad & kExitCombo) == kExitCombo && (pressed & kExitCombo))
                                          : (pressed & kNesBtnSelect) != 0;
        if (exit_pressed) {
            if (playing) {
                // Drop further emu frames first so the list is not painted
                // under a still-running scaler, then ask the emu to stop.
                game_playing_ui_.store(false);
                game_stop_ui_pending_.store(true);
                Schedule(actions_.stop_nes);
                ShowGameSelect();
                game_stop_ui_pending_.store(false);
            } else {
                Schedule(actions_.stop_nes);
                Schedule([] {
                    // Leave game mode entirely when exiting to the app home.
                    Application::GetInstance().SetExternalAudioActive(false);
                });
                OpenApps();
            }
        }
    }
}

void Tab5NativeApps::OpenIcu(int mode, const std::string& external_result) {
    if (!icu_page_) icu_page_ = std::make_unique<Tab5IcuPage>(root_, [this] { OpenApps(); });
    Show(icu_page_->object());
    icu_page_->Open(mode, external_result);
}

void Tab5NativeApps::Close() { lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN); }

bool Tab5NativeApps::IsVisible() const {
    return root_ && !lv_obj_has_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

bool Tab5NativeApps::IsSettingsVisible() const {
    return IsVisible() && !lv_obj_has_flag(settings_page_, LV_OBJ_FLAG_HIDDEN);
}

void Tab5NativeApps::BuildHome() {
    Label(home_page_, "土皮助手", &qd_font_lxgw_36, 0xf5f9fd, 52, 27, 450);
    Label(home_page_, "应用与设置", &qd_font_lxgw_28, 0x9bb7ca, 54, 76, 500);
    Button(home_page_, "返回 Nabo", 1016, 34, 216, 60, [](lv_event_t* event) {
        static_cast<Tab5NativeApps*>(lv_event_get_user_data(event))->Close();
    }, this);

    auto* settings = Card(home_page_, 54, 142, 554, 220, 0x142d43, 0x35627d, 30);
    Card(settings, 25, 24, 7, 48, 0x64cfe8, 0x64cfe8, 3);
    Label(settings, "设置", &qd_font_lxgw_36, 0xf5f9fd, 51, 22, 420);
    Label(settings, "网络 · 亮度 · 音量", &qd_font_lxgw_28,
          0xa8c4d3, 52, 79, 440);
    Button(settings, "打开设置", 52, 142, 450, 58, [](lv_event_t* event) {
        static_cast<Tab5NativeApps*>(lv_event_get_user_data(event))->OpenSettings();
    }, this);

    auto* radio = Card(home_page_, 672, 142, 554, 220, 0x142d43, 0x35627d, 30);
    Card(radio, 25, 24, 7, 48, 0xf7c84d, 0xf7c84d, 3);
    Label(radio, "网络电台", &qd_font_lxgw_36, 0xf5f9fd, 51, 22, 440);
    Label(radio, "选台 · 播放 · 切换", &qd_font_lxgw_28,
          0xa8c4d3, 52, 79, 440);
    Button(radio, "打开电台", 52, 142, 450, 58, [](lv_event_t* event) {
        static_cast<Tab5NativeApps*>(lv_event_get_user_data(event))->OpenRadio();
    }, this);

    auto* icu = Card(home_page_, 54, 389, 554, 239, 0x142d43, 0x35627d, 30);
    Card(icu, 26, 25, 7, 49, 0x89c8a7, 0x89c8a7, 3);
    Label(icu, "ICU 数值工具", &qd_font_lxgw_36, 0xf5f9fd, 52, 23, 420);
    Label(icu, "肾功能 · 氧合 · 血气 · 静脉泵",
          &qd_font_lxgw_28, 0xa8c4d3, 53, 86, 440);
    Button(icu, "打开计算", 52, 155, 450, 58, [](lv_event_t* event) {
        static_cast<Tab5NativeApps*>(lv_event_get_user_data(event))->OpenIcu();
    }, this);

    auto* game = Card(home_page_, 672, 389, 554, 239, 0x142d43, 0x35627d, 30);
    Card(game, 26, 25, 7, 49, 0xe87c64, 0xe87c64, 3);
    Label(game, "红白机 NES", &qd_font_lxgw_36, 0xf5f9fd, 52, 23, 420);
    Label(game, "USB 手柄 · SD 卡 ROM · 720p 适配",
          &qd_font_lxgw_28, 0xa8c4d3, 53, 86, 440);
    Button(game, "进入游戏", 52, 155, 450, 58, [](lv_event_t* event) {
        static_cast<Tab5NativeApps*>(lv_event_get_user_data(event))->OpenNes();
    }, this);

    auto* ir = Card(home_page_, 54, 650, 1172, 50, 0x142d43, 0x35627d, 18);
    Label(ir, "红外遥控 · 电视 / 空调", &qd_font_lxgw_28, 0x9bb7ca, 24, 10, 400);
    lv_obj_add_flag(ir, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ir, [](lv_event_t* event) {
        static_cast<Tab5NativeApps*>(lv_event_get_user_data(event))->OpenIr();
    }, LV_EVENT_CLICKED, this);
}

void Tab5NativeApps::BuildSettings() {
    Label(settings_page_, "设置", &qd_font_lxgw_36, 0xf5f9fd, 52, 27, 440);
    Label(settings_page_, "网络、屏幕与声音", &qd_font_lxgw_28,
          0x9bb7ca, 54, 76, 550);
    Button(settings_page_, "返回应用", 1016, 34, 216, 60, [](lv_event_t* event) {
        static_cast<Tab5NativeApps*>(lv_event_get_user_data(event))->OpenApps();
    }, this);

    auto* network = Card(settings_page_, 54, 139, 1172, 170, 0x142d43, 0x35627d, 27);
    Label(network, "当前网络", &qd_font_lxgw_36, 0xf5f9fd, 34, 24, 560);
    wifi_label_ = Label(network, "正在读取网络状态", &qd_font_lxgw_28,
                        0xa8c4d3, 35, 83, 820);
    lv_obj_set_height(wifi_label_, 75);
    Button(network, "重新配网", 900, 54, 232, 67, [](lv_event_t* event) {
        auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(event));
        lv_label_set_text(self->wifi_label_, "正在切换到配网模式…");
        Schedule(self->actions_.reconfigure_wifi);
    }, this);

    auto* brightness = Card(settings_page_, 54, 332, 562, 256, 0x142d43, 0x35627d, 27);
    Label(brightness, "屏幕亮度", &qd_font_lxgw_36, 0xf5f9fd, 34, 28, 450);
    brightness_value_ = Label(brightness, "70%", &qd_font_lxgw_28,
                              0x83d6e8, 430, 39, 100);
    brightness_slider_ = lv_slider_create(brightness);
    lv_obj_set_pos(brightness_slider_, 42, 135);
    lv_obj_set_size(brightness_slider_, 468, 26);
    lv_slider_set_range(brightness_slider_, 5, 100);
    lv_obj_set_style_bg_color(brightness_slider_, lv_color_hex(0x31546c), LV_PART_MAIN);
    lv_obj_set_style_bg_color(brightness_slider_, lv_color_hex(0x70d0e8), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(brightness_slider_, lv_color_hex(0xf4fbff), LV_PART_KNOB);
    lv_obj_add_event_cb(brightness_slider_, [](lv_event_t* event) {
        const auto code = lv_event_get_code(event);
        if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED) return;
        auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(event));
        const int value = lv_slider_get_value(self->brightness_slider_);
        char text[12];
        std::snprintf(text, sizeof(text), "%d%%", value);
        lv_label_set_text(self->brightness_value_, text);
        if (code == LV_EVENT_RELEASED && self->actions_.set_brightness)
            Schedule([action = self->actions_.set_brightness, value] { action(value); });
    }, LV_EVENT_ALL, this);
    Label(brightness, "轻触滑动，松手后保存。", &qd_font_lxgw_28,
          0x98b5c5, 42, 213, 480);

    auto* volume = Card(settings_page_, 638, 332, 588, 256, 0x142d43, 0x35627d, 27);
    Label(volume, "播放音量", &qd_font_lxgw_36, 0xf5f9fd, 34, 28, 450);
    volume_value_ = Label(volume, "70%", &qd_font_lxgw_28,
                          0x83d6e8, 460, 39, 100);
    volume_slider_ = lv_slider_create(volume);
    lv_obj_set_pos(volume_slider_, 42, 135);
    lv_obj_set_size(volume_slider_, 494, 26);
    lv_slider_set_range(volume_slider_, 0, 100);
    lv_obj_set_style_bg_color(volume_slider_, lv_color_hex(0x31546c), LV_PART_MAIN);
    lv_obj_set_style_bg_color(volume_slider_, lv_color_hex(0xf7c84d), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(volume_slider_, lv_color_hex(0xf4fbff), LV_PART_KNOB);
    lv_obj_add_event_cb(volume_slider_, [](lv_event_t* event) {
        const auto code = lv_event_get_code(event);
        if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED) return;
        auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(event));
        const int value = lv_slider_get_value(self->volume_slider_);
        char text[12];
        std::snprintf(text, sizeof(text), "%d%%", value);
        lv_label_set_text(self->volume_value_, text);
        if (code == LV_EVENT_RELEASED && self->actions_.set_volume)
            Schedule([action = self->actions_.set_volume, value] { action(value); });
    }, LV_EVENT_ALL, this);
    Label(volume, "广播和对话共用此音量。", &qd_font_lxgw_28,
          0x98b5c5, 42, 213, 490);

    // Firmware upgrade: check GitHub releases, stage the image on the SD card, reboot into
    // the updater. Status text and progress are pushed by Tab5Ota via SetFirmwareStatus().
    auto* firmware = Card(settings_page_, 54, 604, 1172, 100, 0x142d43, 0x35627d, 27);
    Label(firmware, "固件升级", &qd_font_cjk_28, 0xf5f9fd, 34, 12, 170);
    fw_status_ = Label(firmware, "当前版本", &qd_font_cjk_28, 0xa8c4d3, 210, 12, 660);
    lv_obj_set_height(fw_status_, 72);
    fw_bar_ = lv_bar_create(firmware);
    lv_obj_set_pos(fw_bar_, 34, 86);
    lv_obj_set_size(fw_bar_, 836, 8);
    lv_bar_set_range(fw_bar_, 0, 100);
    lv_obj_set_style_bg_color(fw_bar_, lv_color_hex(0x31546c), LV_PART_MAIN);
    lv_obj_set_style_bg_color(fw_bar_, lv_color_hex(0x70d0e8), LV_PART_INDICATOR);
    lv_obj_add_flag(fw_bar_, LV_OBJ_FLAG_HIDDEN);
    fw_button_ = Button(firmware, "检查更新", 900, 16, 232, 67, [](lv_event_t* event) {
        auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(event));
        if (self->fw_busy_) return;
        Schedule(self->actions_.firmware_action);
    }, this);
    fw_button_label_ = lv_obj_get_child(fw_button_, 0);
}

void Tab5NativeApps::SetFirmwareStatus(const char* text, const char* button, int progress, bool busy) {
    if (!fw_status_) return;
    lv_label_set_text(fw_status_, text ? text : "");
    if (fw_button_label_) lv_label_set_text(fw_button_label_, button ? button : "");
    fw_busy_ = busy;
    if (fw_button_) lv_obj_set_style_opa(fw_button_, busy ? LV_OPA_50 : LV_OPA_COVER, 0);
    if (fw_bar_) {
        if (progress < 0) {
            lv_obj_add_flag(fw_bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(fw_bar_, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(fw_bar_, std::clamp(progress, 0, 100), LV_ANIM_OFF);
        }
    }
}

void Tab5NativeApps::BuildRadio() {
    Label(radio_page_, "网络电台", &qd_font_lxgw_36, 0xf5f9fd, 52, 27, 440);
    Label(radio_page_, "NABO RADIO  ·  选择频道，或说：我要听广播", &qd_font_lxgw_28,
          0x9bb7ca, 54, 76, 860);
    Button(radio_page_, "返回应用", 1016, 34, 216, 60, [](lv_event_t* event) {
        static_cast<Tab5NativeApps*>(lv_event_get_user_data(event))->OpenApps();
    }, this);

    auto* directory = Card(radio_page_, 48, 139, 380, 522, 0x142d43, 0x35627d, 27);
    Label(directory, "频道", &qd_font_lxgw_36, 0xf5f9fd, 27, 20, 320);
    for (int i = 0; i < kRows; ++i) {
        auto* row = Button(directory, "", 24, 82 + 56 * i, 332, 48,
                           [](lv_event_t* event) {
            auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(event));
            auto* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
            for (int row = 0; row < kRows; ++row) {
                if (self->station_rows_[row] != target) continue;
                const int index = self->station_page_ * kRows + row;
                if (self->actions_.radio_select)
                    Schedule([action = self->actions_.radio_select, index] { action(index); });
                lv_label_set_text(self->radio_state_, "连接中");
                return;
            }
        }, this);
        station_rows_[i] = row;
        station_names_[i] = lv_obj_get_child(row, 0);
        lv_obj_set_pos(station_names_[i], 15, 7);
        lv_obj_set_size(station_names_[i], 302, 34);
        lv_obj_set_style_text_align(station_names_[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(station_names_[i], LV_LABEL_LONG_MODE_DOTS);
    }
    Button(directory, "上一页", 24, 444, 112, 52, [](lv_event_t* event) {
        auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(event));
        self->station_page_ = std::max(0, self->station_page_ - 1);
        self->RefreshStations();
    }, this);
    station_page_label_ = Label(directory, "1 / 1", &qd_font_lxgw_28,
                                0xb5cdd9, 141, 455, 98);
    lv_obj_set_style_text_align(station_page_label_, LV_TEXT_ALIGN_CENTER, 0);
    Button(directory, "下一页", 244, 444, 112, 52, [](lv_event_t* event) {
        auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(event));
        const int count = self->actions_.station_count ? self->actions_.station_count() : 0;
        if ((self->station_page_ + 1) * kRows < count) ++self->station_page_;
        self->RefreshStations();
    }, this);

    auto* player = Card(radio_page_, 448, 139, 784, 522, 0x142d43, 0x35627d, 27);
    lv_obj_set_style_bg_grad_color(player, lv_color_hex(0x1d4056), 0);
    lv_obj_set_style_bg_grad_dir(player, LV_GRAD_DIR_VER, 0);
    Label(player, "NOW PLAYING", &qd_font_lxgw_28, 0x83d6e8, 38, 20, 350);
    auto* ask_song = Button(
        player, "点歌", 600, 12, 156, 50,
        [](lv_event_t* event) {
            auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(event));
            if (!self->actions_.ask_song)
                return;
            lv_label_set_text(self->radio_state_, "请说出想听的歌名…");
            Schedule(self->actions_.ask_song);
        },
        this);
    lv_obj_set_style_bg_color(ask_song, lv_color_hex(0x7a4fd6), 0);
    // Dynamic song/station titles use Noto (much broader CJK) so random
    // NetEase names do not turn into boxes/garbage from the LXGW subset.
    radio_station_ =
        Label(player, "选择一个电台", &font_noto_sans_basic_30_4, 0xf5f9fd, 38, 68, 708);
    lv_obj_set_height(radio_station_, 60);
    radio_state_ = Label(player, "待播放", &qd_font_lxgw_28, 0xf7c84d, 38, 119, 700);
    auto* wave_panel = Card(player, 28, 158, 728, 120, 0x0c2133, 0x346078, 22);
    radio_wave_ = lv_obj_create(wave_panel);
    lv_obj_set_pos(radio_wave_, 18, 10);
    lv_obj_set_size(radio_wave_, 692, 98);
    lv_obj_set_style_bg_opa(radio_wave_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(radio_wave_, 0, 0);
    lv_obj_set_style_pad_all(radio_wave_, 0, 0);
    lv_obj_clear_flag(radio_wave_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(radio_wave_, DrawWave, LV_EVENT_DRAW_MAIN, this);
    for (int i = 0; i < kWaveBars; ++i) {
        wave_heights_[i] = 5;
        wave_colors_[i] = lv_color_hsv_to_rgb((205 + i * 330 / kWaveBars) % 360, 78, 95);
    }
    radio_meta_ = Label(player, "", &qd_font_lxgw_28, 0x9bb7ca, 38, 282, 700);
    lv_obj_set_height(radio_meta_, 28);
    lv_label_set_long_mode(radio_meta_, LV_LABEL_LONG_DOT);
    auto* lyric_panel = Card(player, 28, 311, 728, 132, 0x0c2133, 0x346078, 16);
    // Single current line only — no prev/next stack and no marquee scroll.
    radio_lyric_previous_ = nullptr;
    radio_lyric_next_ = nullptr;
    radio_lyric_ = Label(lyric_panel, "等待歌词", &font_noto_sans_basic_30_4,
                         0xf5f9fd, 18, 42, 692);
    lv_obj_set_height(radio_lyric_, 48);
    lv_label_set_long_mode(radio_lyric_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(radio_lyric_, LV_TEXT_ALIGN_CENTER, 0);
    Button(
        player, "上一台", 28, 449, 168, 62,
        [](lv_event_t* event) {
            auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(event));
            Schedule(self->actions_.radio_previous);
        },
        this);
    auto* play = Button(
        player, "播放", 214, 449, 168, 62,
        [](lv_event_t* event) {
            auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(event));
            Schedule(self->actions_.radio_play_pause);
        },
        this);
    radio_play_label_ = lv_obj_get_child(play, 0);
    lv_obj_set_style_bg_color(play, lv_color_hex(0x1d8493), 0);
    Button(
        player, "停止", 400, 449, 168, 62,
        [](lv_event_t* event) {
            auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(event));
            Schedule(self->actions_.radio_stop);
        },
        this);
    auto* next_button = Button(
        player, "下一台", 586, 449, 168, 62,
        [](lv_event_t* event) {
            auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(event));
            Schedule(self->actions_.radio_next);
        },
        this);
    radio_next_label_ = lv_obj_get_child(next_button, 0);
}

void Tab5NativeApps::DrawWave(lv_event_t* event) {
    auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(event));
    auto* object = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* layer = lv_event_get_layer(event);
    if (!self || !object || !layer) return;

    lv_area_t bounds;
    lv_obj_get_coords(object, &bounds);
    lv_draw_rect_dsc_t bar;
    lv_draw_rect_dsc_init(&bar);
    bar.radius = LV_RADIUS_CIRCLE;
    bar.border_width = 0;
    bar.bg_opa = self->radio_playing_ ? LV_OPA_COVER : LV_OPA_30;
    for (int i = 0; i < kWaveBars; ++i) {
        const int height = self->wave_heights_[i];
        const int x = bounds.x1 + 6 + i * 17;
        const int y = bounds.y1 + lv_obj_get_height(object) / 2 - height / 2;
        bar.bg_color = self->wave_colors_[i];
        lv_area_t area{x, y, x + 9, y + height - 1};
        lv_draw_rect(layer, &bar, &area);
    }
}

void Tab5NativeApps::UpdateWave() {
    if (!IsVisible() || lv_obj_has_flag(radio_page_, LV_OBJ_FLAG_HIDDEN)) return;
    if ((++wave_phase_ & 1U) != 0) return;  // 10 redraws/s, one bounded area.
    const int level = radio_playing_ && actions_.radio_level
        ? std::clamp(actions_.radio_level(), 0, 100) : 0;
    bool changed = false;
    for (int i = 0; i < kWaveBars; ++i) {
        const int ripple = 28 - std::abs((i * 13 + int(wave_phase_) * 5) % 56 - 28);
        const int target = radio_playing_ ? std::clamp(10 + level * (20 + ripple) / 45, 6, 88) : 5;
        const int next = (int(wave_heights_[i]) * 2 + target + 1) / 3;
        changed |= next != wave_heights_[i];
        wave_heights_[i] = static_cast<uint8_t>(next);
    }
    if (changed)
        lv_obj_invalidate(radio_wave_);
}

void Tab5NativeApps::BuildGame() {
    // NES 256×240 → 960×720 (4:3 CRT aspect, 3× vertical integer). Centered
    // on 1280×720 with 160px bars. Integer-ish scale keeps pixels sharp;
    // full 16:9 stretch and PPA bilinear both looked much worse.
    auto* stage = Card(game_page_, 0, 0, 1280, 720, 0x000000, 0x000000, 0);
    lv_obj_set_style_pad_all(stage, 0, 0);
    lv_obj_clear_flag(stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(stage, LV_OBJ_FLAG_CLICKABLE);

    game_src_pixels_ = static_cast<uint16_t*>(
        heap_caps_malloc(256 * 240 * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    constexpr int kW = 960, kH = 720;
    if (game_src_pixels_) {
        memset(game_src_pixels_, 0, 256 * 240 * sizeof(uint16_t));
        const size_t scaled_bytes = size_t(kW) * kH * sizeof(uint16_t);
        bool ok = true;
        for (int i = 0; i < 3; ++i) {
            game_scaled_[i] = static_cast<uint16_t*>(
                heap_caps_malloc(scaled_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!game_scaled_[i]) {
                ok = false;
                break;
            }
            memset(game_scaled_[i], 0, scaled_bytes);
        }
        if (!ok) {
            for (int i = 0; i < 3; ++i) {
                heap_caps_free(game_scaled_[i]);
                game_scaled_[i] = nullptr;
            }
            heap_caps_free(game_src_pixels_);
            game_src_pixels_ = nullptr;
        } else {
            game_write_idx_ = 0;
            game_display_idx_ = 1;
            game_img_.header.magic = LV_IMAGE_HEADER_MAGIC;
            game_img_.header.cf = LV_COLOR_FORMAT_RGB565;
            game_img_.header.w = kW;
            game_img_.header.h = kH;
            game_img_.header.stride = kW * sizeof(uint16_t);
            game_img_.data = reinterpret_cast<const uint8_t*>(game_scaled_[game_display_idx_]);
            game_img_.data_size = scaled_bytes;
            game_canvas_ = lv_image_create(stage);
            lv_obj_set_pos(game_canvas_, (1280 - kW) / 2, 0);
            lv_obj_set_size(game_canvas_, kW, kH);
            lv_image_set_src(game_canvas_, &game_img_);
        }
    }

    game_status_ = Label(stage, "手柄：↑↓ 选 ROM · ○ 开始 · Select 返回",
                         &qd_font_lxgw_28, 0x7f9aad, 24, 16, 900);
    game_rom_label_ = Label(stage, "", &qd_font_lxgw_36, 0xf5f9fd, 24, 64, 1200);
    lv_label_set_long_mode(game_rom_label_, LV_LABEL_LONG_DOT);

    // ROM list (gamepad navigates). Dark card, 8 rows.
    game_select_panel_ = Card(stage, 24, 120, 720, 560, 0x0d1b2b, 0x35627d, 24);
    lv_obj_set_style_bg_opa(game_select_panel_, LV_OPA_80, 0);
    for (int i = 0; i < 8; ++i) {
        auto* row = Card(game_select_panel_, 16, 16 + i * 66, 688, 58, 0x142d43, 0x2a4a62, 12);
        game_rom_rows_[i] = row;
        game_rom_names_[i] = Label(row, "", &qd_font_lxgw_28, 0xf5f9fd, 16, 10, 640);
        lv_label_set_long_mode(game_rom_names_[i], LV_LABEL_LONG_DOT);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }
    RefreshGameRoms();
}

void Tab5NativeApps::ShowGameSelect() {
    game_playing_ui_.store(false);
    if (game_status_) {
        lv_obj_set_pos(game_status_, 24, 16);
        lv_obj_set_width(game_status_, 900);
    }
    if (game_rom_label_) {
        lv_obj_remove_flag(game_rom_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (game_select_panel_) {
        lv_obj_remove_flag(game_select_panel_, LV_OBJ_FLAG_HIDDEN);
    }
    if (game_status_) {
        lv_label_set_text(game_status_, "手柄：↑↓ 选 ROM · ○ 开始 · Select 返回");
    }
    if (game_canvas_ && tab5_nes_video::Available()) {
        lv_obj_add_flag(game_canvas_, LV_OBJ_FLAG_HIDDEN);
    }
    RefreshGameRoms();
}

void Tab5NativeApps::ShowGamePlay() {
    game_playing_ui_.store(true);
    if (game_rom_label_) {
        lv_obj_add_flag(game_rom_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (game_select_panel_) {
        lv_obj_add_flag(game_select_panel_, LV_OBJ_FLAG_HIDDEN);
    }
    if (tab5_nes_video::Available()) {
        // The emulator writes the centre of the panel directly; keep LVGL's
        // image hidden and park a short hint in the left bar (x < 160 stays
        // visible in both the 3x and the 4:3 layout).
        if (game_canvas_) {
            lv_obj_add_flag(game_canvas_, LV_OBJ_FLAG_HIDDEN);
        }
        if (game_status_) {
            lv_obj_set_pos(game_status_, 10, 200);
            lv_obj_set_width(game_status_, 144);
            lv_label_set_text(game_status_, "Select\n+Start\n返回列表\n\nSelect\n+左/右\n切换画面");
        }
    } else if (game_status_) {
        lv_label_set_text(game_status_, "游戏中 · Select+Start 返回");
    }
}

void Tab5NativeApps::RefreshGameRoms() {
    if (!game_select_panel_) return;
    const int count = actions_.nes_rom_count ? actions_.nes_rom_count() : 0;
    const int sel = actions_.nes_rom_index ? actions_.nes_rom_index() : 0;
    if (game_rom_label_ && actions_.nes_rom_name) {
        lv_label_set_text(game_rom_label_, actions_.nes_rom_name(sel).c_str());
    }
    if (count <= 0) {
        if (game_status_) {
            lv_label_set_text(game_status_, "SD /nes 未找到 ROM");
        }
        return;
    }
    // Window of 8 rows centered on selection.
    const int rows = 8;
    int start = sel - rows / 2;
    if (start < 0) start = 0;
    if (start + rows > count) start = count > rows ? count - rows : 0;
    for (int i = 0; i < rows; ++i) {
        if (!game_rom_rows_[i]) continue;
        const int idx = start + i;
        if (idx >= count) {
            lv_obj_add_flag(game_rom_rows_[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(game_rom_rows_[i], LV_OBJ_FLAG_HIDDEN);
        if (game_rom_names_[i] && actions_.nes_rom_name) {
            char text[80];
            snprintf(text, sizeof(text), "%s%s", idx == sel ? "▶ " : "",
                     actions_.nes_rom_name(idx).c_str());
            lv_label_set_text(game_rom_names_[i], text);
        }
        lv_obj_set_style_bg_color(
            game_rom_rows_[i],
            lv_color_hex(idx == sel ? 0x1d8493 : 0x142d43), 0);
        lv_obj_set_style_border_color(
            game_rom_rows_[i],
            lv_color_hex(idx == sel ? 0x64cfe8 : 0x2a4a62), 0);
    }
}

void Tab5NativeApps::DrawNes(lv_event_t* event) { (void)event; }

void Tab5NativeApps::SetNesFrame(const uint16_t* pixels, uint16_t width, uint16_t height) {
    if (!pixels || !game_canvas_) return;
    if (width != 256 || height != 240) return;
    if (!game_scaled_[0] || !game_scaled_[1] || !game_scaled_[2]) return;
    if (!IsVisible() || lv_obj_has_flag(game_page_, LV_OBJ_FLAG_HIDDEN)) return;
    if (!game_playing_ui_.load()) return;

    // Scale into the free write slot (never the one LVGL is drawing).
    auto* dst = game_scaled_[game_write_idx_];
    // 256×240 → 960×720. Vertical is exact 3×; horizontal 3.75× keeps CRT 4:3
    // without the mushy 5×3 / PPA stretch. Nearest neighbor, sharp pixels.
    constexpr int kDw = 960;
    for (int y = 0; y < 240; ++y) {
        const uint16_t* srow = pixels + size_t(y) * 256;
        uint16_t* r0 = dst + size_t(y * 3) * kDw;
        for (int x = 0; x < 256; ++x) {
            const uint16_t c = srow[x];
            const int x0 = (x * kDw) / 256;
            const int x1 = ((x + 1) * kDw) / 256;
            for (int dx = x0; dx < x1; ++dx) r0[dx] = c;
        }
        memcpy(r0 + kDw, r0, size_t(kDw) * sizeof(uint16_t));
        memcpy(r0 + 2 * kDw, r0, size_t(kDw) * sizeof(uint16_t));
    }

    // Publish immediately under the LVGL lock so every emu frame can show.
    // Triple buffer keeps the displayed surface immutable while we draw the
    // next one on the other slots.
    const int ready = game_write_idx_;
    const int previous = game_display_idx_;
    game_display_idx_ = ready;
    game_write_idx_ = previous >= 0 ? previous : (ready + 1) % 3;
    if (lvgl_port_lock(2)) {
        game_img_.data = reinterpret_cast<const uint8_t*>(game_scaled_[ready]);
        lv_image_set_src(game_canvas_, &game_img_);
        lv_obj_invalidate(game_canvas_);
        lvgl_port_unlock();
    }
}

void Tab5NativeApps::SetNesStatus(const char* status) {
    if (!game_status_) return;
    // Keep the in-game hint; "Loading/Playing" states arrive right after it.
    if (game_playing_ui_.load() && tab5_nes_video::Available()) return;
    lv_label_set_text(game_status_, status && *status ? status : "—");
}

void Tab5NativeApps::SetNesPlaying(bool playing) {
    if (playing) {
        ShowGamePlay();
    } else {
        // Emulator idle/failed/stopped — restore the ROM list.
        game_playing_ui_.store(false);
        game_pad_resync_ = true;
        ShowGameSelect();
    }
}

void Tab5NativeApps::SetMusicLyric(const char* title, const char* artist, const char* line) {
    SetMusicLyricsWindow(title, artist, "", line, "");
}

void Tab5NativeApps::SetMusicLyricLine(const char* line) {
    // Lyrics only — never touch the NOW PLAYING title (that is the station/song name).
    if (radio_lyric_)
        lv_label_set_text(radio_lyric_, line && *line ? line : "等待歌词");
}

void Tab5NativeApps::SetMusicLyricsWindow(const char* title, const char* artist,
                                          const char* previous, const char* current,
                                          const char* next) {
    const lv_font_t* font = MusicTextFont();
    if (music_font_ != font) {
        music_font_ = font;
        if (radio_station_)
            lv_obj_set_style_text_font(radio_station_, font, 0);
        for (auto* lyric : {radio_lyric_previous_, radio_lyric_, radio_lyric_next_}) {
            if (lyric)
                lv_obj_set_style_text_font(lyric, font, 0);
        }
    }
    if (title && *title) {
        char head[96];
        std::snprintf(head, sizeof(head), "%s%s%s", title, (artist && *artist) ? " - " : "",
                      (artist && *artist) ? artist : "");
        if (radio_station_)
            lv_label_set_text(radio_station_, head);
        if (radio_next_label_)
            lv_label_set_text(radio_next_label_, "下一首");
    }
    SetMusicLyricLine(current);
}

void Tab5NativeApps::ClearMusicLyrics() {
    if (radio_next_label_)
        lv_label_set_text(radio_next_label_, "下一台");
    if (radio_lyric_previous_)
        lv_label_set_text(radio_lyric_previous_, "");
    if (radio_lyric_)
        lv_label_set_text(radio_lyric_, "等待歌词");
    if (radio_lyric_next_)
        lv_label_set_text(radio_lyric_next_, "");
}

void Tab5NativeApps::RefreshWifi() {
    if (actions_.wifi_summary)
        lv_label_set_text(wifi_label_, actions_.wifi_summary().c_str());
}

void Tab5NativeApps::RefreshSettings() {
    RefreshWifi();
    if (actions_.get_brightness) {
        const int value = std::clamp(actions_.get_brightness(), 5, 100);
        lv_slider_set_value(brightness_slider_, value, LV_ANIM_OFF);
        char text[12];
        std::snprintf(text, sizeof(text), "%d%%", value);
        lv_label_set_text(brightness_value_, text);
    }
    if (actions_.get_volume) {
        const int value = std::clamp(actions_.get_volume(), 0, 100);
        lv_slider_set_value(volume_slider_, value, LV_ANIM_OFF);
        char text[12];
        std::snprintf(text, sizeof(text), "%d%%", value);
        lv_label_set_text(volume_value_, text);
    }
}

void Tab5NativeApps::RefreshStations() {
    const int count = actions_.station_count ? std::max(0, actions_.station_count()) : 0;
    const int pages = std::max(1, (count + kRows - 1) / kRows);
    station_page_ = std::clamp(station_page_, 0, pages - 1);
    for (int row = 0; row < kRows; ++row) {
        const int index = station_page_ * kRows + row;
        if (index >= count) {
            lv_obj_add_flag(station_rows_[row], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(station_rows_[row], LV_OBJ_FLAG_HIDDEN);
        const std::string name = actions_.station_name ? actions_.station_name(index) : "";
        char text[120];
        std::snprintf(text, sizeof(text), "%02d  %s", index + 1, name.c_str());
        lv_label_set_text(station_names_[row], text);
        const bool selected = name == current_station_name_;
        lv_obj_set_style_bg_color(station_rows_[row],
                                  lv_color_hex(selected ? 0x397b87 : 0x254c66), 0);
        lv_obj_set_style_border_color(station_rows_[row],
                                      lv_color_hex(selected ? 0x91dfde : 0x3f7590), 0);
    }
    char page[32];
    std::snprintf(page, sizeof(page), "%d / %d", station_page_ + 1, pages);
    lv_label_set_text(station_page_label_, page);
}

void Tab5NativeApps::SetRadioState(const char* station, const char* state,
                                   const char* meta) {
    // Must be called with the display lock held. Never Schedule LVGL work to
    // main without that lock: lv_inv_area asserts if invalidate runs during
    // rendering and wedges the main task (watchdog + dead play queue).
    const bool station_changed = station && *station &&
                                 current_station_name_ != station;
    const std::string state_key = state ? state : "";
    const bool state_changed = state_key != current_radio_state_key_;
    if (station && *station) {
        current_station_name_ = station;
        // Always refresh the title so selecting a station cannot leave it blank
        // after a music track previously owned this label.
        lv_label_set_text(radio_station_, station);
    }
    if (meta) lv_label_set_text(radio_meta_, meta);
    const char* localized = "待播放";
    if (state) {
        if (std::strcmp(state, "Playing") == 0) localized = "播放中";
        else if (std::strcmp(state, "Buffering") == 0) localized = "缓冲中";
        else if (std::strcmp(state, "Connecting") == 0 ||
                 std::strcmp(state, "Reconnecting") == 0) localized = "连接中";
        else if (std::strcmp(state, "Paused") == 0) localized = "已暂停";
        else if (std::strcmp(state, "Stopped") == 0) localized = "已停止";
        else if (std::strcmp(state, "Error") == 0) localized = "播放失败";
        else if (std::strcmp(state, "Waiting WiFi") == 0) localized = "等待网络";
        else if (std::strcmp(state, "Unavailable") == 0) localized = "暂不可用";
    }
    lv_label_set_text(radio_state_, localized);
    radio_playing_ = state && (std::strcmp(state, "Playing") == 0 ||
                               std::strcmp(state, "Buffering") == 0 ||
                               std::strcmp(state, "Connecting") == 0);
    lv_label_set_text(radio_play_label_, radio_playing_ ? "暂停" : "播放");
    // Directory rebuild is expensive and floods invalidate areas; only do it
    // when the selection or playback phase actually changes.
    if (station_changed || state_changed) {
        current_radio_state_key_ = state_key;
        RefreshStations();
    }
}
