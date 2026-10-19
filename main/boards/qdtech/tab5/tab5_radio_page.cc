// NABO FM radio page of Tab5NativeApps: studio on the left, tuner on the right.
// Layout follows the 1280x720 NABO FM mockup (午夜琥珀 palette).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "display/lvgl_display/lvgl_font.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "esp_log.h"
#include "font/binfont_loader/lv_binfont_loader.h"
#include "tab5_native_apps.h"
#include "tab5_sd.h"
#include "tab5_ui_motion.h"

LV_FONT_DECLARE(qd_font_lxgw_28);
LV_FONT_DECLARE(qd_font_cjk_28);

namespace {

const char* TAG = "RadioPage";
constexpr uint32_t kPage = 0x080b12, kText = 0xf5efe4, kMuted = 0x8b93a5, kDimmer = 0x4c5568,
                   kAmber = 0xf7b955, kAmberDim = 0x6e5025, kDialBg = 0x171107,
                   kDialLine = 0x3a2a12, kRed = 0xff3b30, kLevel = 0x62e3c6, kHeart = 0xff6f91,
                   kPanel = 0x131a28, kChip = 0x1a2131, kChipPressed = 0x273149, kVoice = 0xc9b8ff;
constexpr int kColX = 616, kColW = 640;
constexpr int kDialY = 386, kDialH = 150;
constexpr float kStationGap = 64;  // dial pixels per station
constexpr uint32_t kTuneRestMs = 500;
constexpr const char* kStationFontFile = "/sdcard/tab5/nabo/radio-v1/wenkai_56.bin";
constexpr const char* kStationFontLvgl = "S:tab5/nabo/radio-v1/wenkai_56.bin";
constexpr const char* kTuneHint = "拖动刻度盘换台，停手半秒自动连接";
constexpr const char* kVoiceHint = "也可以直接说：Nabo，换个音乐台";

std::shared_ptr<LvglFont> MusicTextFontOwner() {
    auto* theme = LvglThemeManager::GetInstance().GetTheme("dark");
    if (theme) {
        auto font = theme->GetTextFont();
        if (font && font->font())
            return font;
    }
    return nullptr;
}

lv_obj_t* Plain(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color, int radius) {
    auto* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

// A flat touch target in the radio palette, with the shared press feel.
lv_obj_t* Pad(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color, int radius,
              lv_event_cb_t callback, void* user_data) {
    auto* o = Plain(parent, x, y, w, h, color, radius);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(o, lv_color_hex(color == kAmber ? 0xffd690 : kChipPressed),
                              LV_STATE_PRESSED);
    tab5_ui::AddPressFeel(o);
    lv_obj_add_event_cb(o, callback, LV_EVENT_CLICKED, user_data);
    return o;
}

lv_obj_t* Glyph(lv_obj_t* parent, const char* symbol, const lv_font_t* font, uint32_t color) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_center(label);
    return label;
}

void FillHeart(lv_layer_t* layer, int cx, int cy, int r, uint32_t color) {
    lv_draw_rect_dsc_t round;
    lv_draw_rect_dsc_init(&round);
    round.radius = LV_RADIUS_CIRCLE;
    round.bg_color = lv_color_hex(color);
    for (int side : {-1, 1}) {
        const int x = cx + side * r * 9 / 10;
        lv_area_t area{x - r, cy - r * 3 / 2, x + r, cy + r / 2};
        lv_draw_rect(layer, &round, &area);
    }
    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.color = lv_color_hex(color);
    tri.p[0] = {lv_value_precise_t(cx - r * 19 / 10), lv_value_precise_t(cy - r / 5)};
    tri.p[1] = {lv_value_precise_t(cx + r * 19 / 10), lv_value_precise_t(cy - r / 5)};
    tri.p[2] = {lv_value_precise_t(cx), lv_value_precise_t(cy + r * 2)};
    lv_draw_triangle(layer, &tri);
}

}  // namespace

void Tab5NativeApps::BuildRadio() {
    lv_obj_set_style_bg_color(radio_page_, lv_color_hex(kPage), 0);
    lv_obj_set_style_border_color(radio_page_, lv_color_hex(kPage), 0);

    Label(radio_page_, "NABO FM", &qd_font_lxgw_28, kAmber, 24, 12, 150);
    Label(radio_page_, "网络电台", &qd_font_cjk_28, kMuted, 162, 14, 140);
    // Voice status (聆听中 / 回应中 / last utterance) in the status bar.
    radio_voice_ = Label(radio_page_, "", &qd_font_cjk_28, kVoice, 520, 12, 548);
    lv_obj_set_height(radio_voice_, 40);
    lv_label_set_long_mode(radio_voice_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(radio_voice_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_flag(radio_voice_, LV_OBJ_FLAG_HIDDEN);
    auto* back = Pad(
        radio_page_, 1088, 8, 168, 50, kChip, 25,
        [](lv_event_t* event) {
            static_cast<Tab5NativeApps*>(lv_event_get_user_data(event))->OpenApps();
        },
        this);
    Glyph(back, "返回应用", &qd_font_cjk_28, kText);

    radio_stage_ = std::make_unique<Tab5RadioStage>(radio_page_, 24, 68);

    // Now playing.
    radio_eyebrow_ = Label(radio_page_, "", &qd_font_cjk_28, kMuted, kColX, 70, 540);
    lv_obj_set_height(radio_eyebrow_, 40);
    lv_label_set_long_mode(radio_eyebrow_, LV_LABEL_LONG_DOT);
    radio_station_ = Label(radio_page_, "选择一个电台", &qd_font_cjk_28, kText, kColX, 110, 548);
    lv_obj_set_height(radio_station_, 78);
    lv_label_set_long_mode(radio_station_, LV_LABEL_LONG_DOT);
    radio_state_ = Label(radio_page_, "待播放", &qd_font_cjk_28, kMuted, kColX, 192, 190);
    lv_obj_set_height(radio_state_, 40);
    radio_meta_ = Label(radio_page_, "", &qd_font_cjk_28, kDimmer, kColX + 190, 192, 450);
    lv_obj_set_height(radio_meta_, 40);
    lv_label_set_long_mode(radio_meta_, LV_LABEL_LONG_DOT);
    radio_fav_ = Pad(
        radio_page_, kColX + kColW - 76, 96, 76, 76, kChip, LV_RADIUS_CIRCLE,
        [](lv_event_t* event) {
            auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(event));
            if (self->radio_band_ < 0 || !self->actions_.toggle_favorite)
                return;
            const auto& band = self->radio_band_list_[self->radio_band_].stations;
            const int at = int(std::lround(self->dial_pos_));
            if (at < 0 || at >= int(band.size()))
                return;
            const int station = band[at];
            if (station >= int(self->radio_favorites_.size()))
                return;
            const bool now_favorite = !self->radio_favorites_[station];
            self->radio_favorites_[station] = now_favorite;
            Schedule([toggle = self->actions_.toggle_favorite, station] { toggle(station); });
            if (now_favorite && self->radio_stage_)
                self->radio_stage_->Favorite(lv_tick_get());
            self->RebuildRadioBands(false);
            self->RefreshTuner();
        },
        this);
    lv_obj_add_event_cb(radio_fav_, DrawHeart, LV_EVENT_DRAW_MAIN_END, this);

    // Hint or the current lyric line.
    auto* strip = Plain(radio_page_, kColX, 246, kColW, 64, kPanel, 16);
    radio_strip_tag_ = Label(strip, "提示", &qd_font_cjk_28, kAmber, 20, 12, 64);
    radio_lyric_ = Label(strip, kTuneHint, &qd_font_cjk_28, kMuted, 88, 10, 532);
    lv_obj_set_height(radio_lyric_, 44);
    lv_label_set_long_mode(radio_lyric_, LV_LABEL_LONG_DOT);

    // Bands; chips are created by RebuildRadioBands.
    radio_bands_ = lv_obj_create(radio_page_);
    lv_obj_remove_style_all(radio_bands_);
    lv_obj_set_pos(radio_bands_, kColX, 322);
    lv_obj_set_size(radio_bands_, kColW, 52);
    lv_obj_set_flex_flow(radio_bands_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(radio_bands_, 10, 0);
    lv_obj_set_scroll_dir(radio_bands_, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(radio_bands_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(radio_bands_, LV_OBJ_FLAG_GESTURE_BUBBLE);

    radio_dial_ = Plain(radio_page_, kColX, kDialY, kColW, kDialH, kDialBg, 18);
    lv_obj_set_style_border_width(radio_dial_, 1, 0);
    lv_obj_set_style_border_color(radio_dial_, lv_color_hex(kDialLine), 0);
    lv_obj_add_flag(radio_dial_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(radio_dial_, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(radio_dial_, DrawDial, LV_EVENT_DRAW_MAIN_END, this);
    for (auto code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
        lv_obj_add_event_cb(radio_dial_, OnDial, code, this);
    auto* needle = Plain(radio_dial_, kColW / 2 - 2, 6, 3, kDialH - 14, kRed, 2);
    lv_obj_remove_flag(needle, LV_OBJ_FLAG_CLICKABLE);
    auto* cap = Plain(radio_dial_, kColW / 2 - 8, 2, 15, 15, kRed, LV_RADIUS_CIRCLE);
    lv_obj_remove_flag(cap, LV_OBJ_FLAG_CLICKABLE);

    // Neighbours under the dial: tap one to move there.
    auto neighbour = [this](int x, int width, lv_text_align_t align, int delta) {
        auto* label = Label(radio_page_, "", &qd_font_cjk_28, kMuted, x, 542, width);
        lv_obj_set_height(label, 40);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(label, align, 0);
        lv_obj_add_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(label, 8);
        lv_obj_add_event_cb(
            label,
            delta < 0 ? static_cast<lv_event_cb_t>([](lv_event_t* e) {
                static_cast<Tab5NativeApps*>(lv_event_get_user_data(e))->StepStation(-1);
            })
                      : static_cast<lv_event_cb_t>([](lv_event_t* e) {
                            static_cast<Tab5NativeApps*>(lv_event_get_user_data(e))->StepStation(1);
                        }),
            LV_EVENT_CLICKED, this);
        return label;
    };
    radio_prev_name_ = neighbour(kColX, 250, LV_TEXT_ALIGN_LEFT, -1);
    radio_next_name_ = neighbour(kColX + kColW - 250, 250, LV_TEXT_ALIGN_RIGHT, 1);
    radio_count_ = Label(radio_page_, "", &lv_font_montserrat_20, kAmberDim, kColX + 250, 550, 140);
    lv_obj_set_style_text_align(radio_count_, LV_TEXT_ALIGN_CENTER, 0);

    // Transport.
    auto* prev = Pad(
        radio_page_, kColX, 600, 84, 84, kChip, 22,
        [](lv_event_t* e) {
            static_cast<Tab5NativeApps*>(lv_event_get_user_data(e))->StepStation(-1);
        },
        this);
    Glyph(prev, LV_SYMBOL_PREV, &lv_font_montserrat_48, kText);
    auto* play = Pad(
        radio_page_, kColX + 100, 588, 108, 108, kAmber, LV_RADIUS_CIRCLE,
        [](lv_event_t* e) {
            auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(e));
            Schedule(self->actions_.radio_play_pause);
        },
        this);
    radio_play_label_ = Glyph(play, LV_SYMBOL_PLAY, &lv_font_montserrat_48, kDialBg);
    auto* next = Pad(
        radio_page_, kColX + 224, 600, 84, 84, kChip, 22,
        [](lv_event_t* e) {
            static_cast<Tab5NativeApps*>(lv_event_get_user_data(e))->StepStation(1);
        },
        this);
    Glyph(next, LV_SYMBOL_NEXT, &lv_font_montserrat_48, kText);
    radio_next_label_ = nullptr;

    auto tool = [this](int x, const char* symbol, const char* text, uint32_t color,
                       lv_event_cb_t callback) {
        auto* pad = Pad(radio_page_, x, 600, 150, 84, kChip, 22, callback, this);
        auto* icon = Glyph(pad, symbol, &lv_font_montserrat_20, color);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 22, 0);
        auto* label = Glyph(pad, text, &qd_font_cjk_28, kText);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 56, 0);
        return label;
    };
    ask_song_label_ = tool(kColX + kColW - 310, LV_SYMBOL_AUDIO, "点歌", kVoice, [](lv_event_t* e) {
        auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(e));
        if (!self->actions_.ask_song)
            return;
        lv_label_set_text(self->radio_state_, "请说出想听的歌名…");
        self->SetVoiceStatus("正在连接 Nabo…", true);
        Schedule(self->actions_.ask_song);
    });
    tool(kColX + kColW - 150, LV_SYMBOL_STOP, "停止", kText, [](lv_event_t* e) {
        auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(e));
        Schedule(self->actions_.radio_stop);
    });
}

void Tab5NativeApps::OpenRadio(bool start_playback) {
    const bool was_visible = IsRadioVisible();
    // The theme font can be replaced by the SD font after startup, so bind the
    // current owner before LVGL lays out this page.
    SyncRadioTextFont();
    LoadStationFont();
    Show(radio_page_);
    if (!was_visible) {
        RefreshStations();
        if (radio_stage_)
            radio_stage_->Open(lv_tick_get());
    }
    if (start_playback)
        Schedule(actions_.start_radio);
}

void Tab5NativeApps::LoadStationFont() {
    static bool attempted = false;
    if (station_font_ || attempted || !Tab5SdReady())
        return;
    attempted = true;
    FILE* file = std::fopen(kStationFontFile, "rb");
    if (!file) {
        ESP_LOGI(TAG, "no station font on the card; using the text font");
        return;
    }
    std::fclose(file);
    station_font_ = lv_binfont_create(kStationFontLvgl);
    if (!station_font_) {
        ESP_LOGW(TAG, "station font rejected");
        return;
    }
    station_font_->fallback = music_font_ ? music_font_ : &qd_font_cjk_28;
    lv_obj_set_style_text_font(radio_station_, station_font_, 0);
}

void Tab5NativeApps::SyncRadioTextFont() {
    auto next_owner = MusicTextFontOwner();
    const lv_font_t* font = next_owner ? next_owner->font() : &qd_font_cjk_28;
    if (music_font_ == font)
        return;
    // A new theme can replace the previous shared owner while this page is hidden.
    // Keep it alive until every LVGL style has been rebound to the new font.
    auto previous_owner = std::move(music_font_owner_);
    music_font_owner_ = std::move(next_owner);
    music_font_ = font;
    if (station_font_)
        station_font_->fallback = font;
    else if (radio_station_)
        lv_obj_set_style_text_font(radio_station_, font, 0);
    if (radio_lyric_ && radio_song_) {
        lv_obj_set_style_text_font(radio_lyric_, font, 0);
        lyric_font_ = font;
    }
    // Neighbour names may come from radio.json on the card; use the broad font.
    for (auto* label : {radio_prev_name_, radio_next_name_})
        if (label)
            lv_obj_set_style_text_font(label, font, 0);
}

void Tab5NativeApps::SetMusicLyric(const char* title, const char* artist, const char* line) {
    SetMusicLyricsWindow(title, artist, "", line, "");
}

void Tab5NativeApps::SetMusicLyricLine(const char* line) {
    // Lyrics only; never the title (that is the station or song name).
    if (!radio_lyric_)
        return;
    // Setting a style repaints the label even when the value is the same, and
    // status updates arrive every second or two: restyle only on a mode change.
    const lv_font_t* font = radio_song_ && music_font_ ? music_font_ : &qd_font_cjk_28;
    if (lyric_font_ != font || lyric_song_style_ != radio_song_) {
        lyric_font_ = font;
        lyric_song_style_ = radio_song_;
        lv_obj_set_style_text_font(radio_lyric_, font, 0);
        lv_obj_set_style_text_color(radio_lyric_, lv_color_hex(radio_song_ ? kText : kMuted), 0);
    }
    if (!radio_song_) {
        SetLabelTextIfChanged(radio_strip_tag_, "提示");
        const bool live = radio_link_state_ == "Playing";
        SetLabelTextIfChanged(radio_lyric_, live ? kVoiceHint : kTuneHint);
        return;
    }
    SetLabelTextIfChanged(radio_strip_tag_, "歌词");
    SetLabelTextIfChanged(radio_lyric_, line && *line ? line : "等待歌词");
}

void Tab5NativeApps::SetMusicLyricsWindow(const char* title, const char* artist, const char*,
                                          const char* current, const char*) {
    if (IsRadioVisible())
        SyncRadioTextFont();
    if (title && *title) {
        radio_song_ = true;
        SetLabelTextIfChanged(radio_station_, title);
        std::string eyebrow = "点歌";
        if (artist && *artist)
            eyebrow += std::string(" · ") + artist;
        SetLabelTextIfChanged(radio_eyebrow_, eyebrow.c_str());
        lv_obj_set_style_opa(radio_dial_, LV_OPA_50, 0);
    }
    SetMusicLyricLine(current);
}

void Tab5NativeApps::SetVoiceStatus(const char* text, bool active) {
    if (!radio_voice_)
        return;
    if (!text || !*text) {
        lv_obj_add_flag(radio_voice_, LV_OBJ_FLAG_HIDDEN);
    } else {
        SetLabelTextIfChanged(radio_voice_, text);
        lv_obj_clear_flag(radio_voice_, LV_OBJ_FLAG_HIDDEN);
    }
    SetLabelTextIfChanged(ask_song_label_, active ? "对话中" : "点歌");
}

void Tab5NativeApps::ClearMusicLyrics() {
    radio_song_ = false;
    if (radio_dial_)
        lv_obj_set_style_opa(radio_dial_, LV_OPA_COVER, 0);
    SetMusicLyricLine(nullptr);
    RefreshTuner();
}

void Tab5NativeApps::RefreshStations() {
    RebuildRadioBands();
    FollowCurrentStation();
    RefreshTuner();
}

void Tab5NativeApps::RebuildRadioBands(bool reload_favorites) {
    const int count = actions_.station_count ? std::max(0, actions_.station_count()) : 0;
    // After a heart tap the queued toggle has not reached the service yet; keep
    // the local copy instead of reading the old value back.
    if (reload_favorites || int(radio_favorites_.size()) != count) {
        radio_favorites_.assign(count, 0);
        for (int i = 0; i < count; ++i)
            radio_favorites_[i] = actions_.station_favorite && actions_.station_favorite(i);
    }
    // 收藏, the service's bands (最近, 本地, 新闻, ...), then 全部.
    std::vector<RadioBand> bands{{"收藏", {}}};
    for (int i = 0; i < count; ++i)
        if (radio_favorites_[i])
            bands[0].stations.push_back(uint16_t(i));
    if (actions_.station_bands) {
        for (auto& band : actions_.station_bands()) {
            band.stations.erase(std::remove_if(band.stations.begin(), band.stations.end(),
                                               [count](uint16_t s) { return s >= count; }),
                                band.stations.end());
            bands.push_back(std::move(band));
        }
    }
    RadioBand all{"全部", {}};
    all.stations.reserve(count);
    for (int i = 0; i < count; ++i)
        all.stations.push_back(uint16_t(i));
    bands.push_back(std::move(all));

    const std::string previous = radio_band_ >= 0 && radio_band_ < int(radio_band_list_.size())
                                     ? radio_band_list_[radio_band_].name
                                     : "";
    bool same_names = bands.size() == radio_band_list_.size();
    for (size_t i = 0; same_names && i < bands.size(); ++i)
        same_names = bands[i].name == radio_band_list_[i].name;
    radio_band_list_ = std::move(bands);
    radio_band_ = -1;
    for (size_t i = 0; i < radio_band_list_.size(); ++i)
        if (radio_band_list_[i].name == previous)
            radio_band_ = int(i);
    if (radio_band_ < 0)
        radio_band_ = int(radio_band_list_.size()) - 1;

    if (!same_names) {
        lv_obj_clean(radio_bands_);
        for (size_t i = 0; i < radio_band_list_.size(); ++i) {
            auto* chip = lv_obj_create(radio_bands_);
            lv_obj_remove_style_all(chip);
            lv_obj_set_size(chip, LV_SIZE_CONTENT, 48);
            lv_obj_set_style_pad_hor(chip, 22, 0);
            lv_obj_set_style_radius(chip, 24, 0);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
            lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
            tab5_ui::AddPressFeel(chip);
            auto* label = lv_label_create(chip);
            lv_label_set_text(label, radio_band_list_[i].name.c_str());
            lv_obj_set_style_text_font(label, &qd_font_cjk_28, 0);
            lv_obj_center(label);
            lv_obj_set_user_data(chip, reinterpret_cast<void*>(i));
            lv_obj_add_event_cb(
                chip,
                [](lv_event_t* e) {
                    auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(e));
                    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                    self->SelectRadioBand(
                        int(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target))), true);
                },
                LV_EVENT_CLICKED, this);
        }
    }
    SelectRadioBand(radio_band_, false);
}

void Tab5NativeApps::SelectRadioBand(int band, bool tune) {
    if (band < 0 || band >= int(radio_band_list_.size()))
        return;
    radio_band_ = band;
    const auto& stations = radio_band_list_[band].stations;
    const int at = BandPosition(radio_current_);
    lv_anim_delete(this, nullptr);
    dial_gliding_ = false;
    SetDialPosition(at >= 0 ? at : 0);
    // A real band switch lands on its first station; staying in band keeps playing.
    if (tune && at < 0 && !stations.empty()) {
        tune_index_ = stations.front();
        ScheduleTune();
    }
    const uint32_t count = lv_obj_get_child_count(radio_bands_);
    for (uint32_t i = 0; i < count; ++i) {
        auto* chip = lv_obj_get_child(radio_bands_, i);
        const bool on = int(i) == band;
        lv_obj_set_style_bg_color(chip, lv_color_hex(on ? kAmber : kChip), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(chip, 0), lv_color_hex(on ? kDialBg : kMuted),
                                    0);
        if (on)
            lv_obj_scroll_to_view(chip, LV_ANIM_ON);
    }
    RefreshTuner();
}

int Tab5NativeApps::BandPosition(int station) const {
    if (radio_band_ < 0 || station < 0)
        return -1;
    const auto& stations = radio_band_list_[radio_band_].stations;
    auto it = std::find(stations.begin(), stations.end(), station);
    return it == stations.end() ? -1 : int(it - stations.begin());
}

void Tab5NativeApps::SetDialPosition(float position) {
    const int previous = int(std::lround(dial_pos_));
    dial_pos_ = position;
    if (radio_dial_)
        lv_obj_invalidate(radio_dial_);
    if (radio_stage_)
        radio_stage_->SetKnob(position);
    if (int(std::lround(position)) != previous)
        RefreshTuner();
}

void Tab5NativeApps::GlideDial(int target) {
    lv_anim_delete(this, nullptr);
    if (!IsRadioVisible()) {
        SetDialPosition(target);
        return;
    }
    dial_gliding_ = true;
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, this);
    lv_anim_set_values(&anim, int32_t(dial_pos_ * 1000), target * 1000);
    lv_anim_set_duration(&anim, 180);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, [](void* var, int32_t value) {
        static_cast<Tab5NativeApps*>(var)->SetDialPosition(value / 1000.0f);
    });
    lv_anim_set_completed_cb(
        &anim, [](lv_anim_t* a) { static_cast<Tab5NativeApps*>(a->var)->dial_gliding_ = false; });
    lv_anim_start(&anim);
}

void Tab5NativeApps::StepStation(int delta) {
    if (radio_band_ < 0)
        return;
    int band = radio_band_;
    int target = int(std::lround(dial_pos_)) + delta;
    if (target < 0 || target >= int(radio_band_list_[band].stations.size())) {
        // Past either end the dial rolls into the neighbouring band that has stations.
        const int bands = int(radio_band_list_.size());
        for (int i = 0; i < bands; ++i) {
            band = (band + (delta > 0 ? 1 : bands - 1)) % bands;
            if (!radio_band_list_[band].stations.empty())
                break;
        }
        const auto& stations = radio_band_list_[band].stations;
        if (stations.empty())
            return;
        target = delta > 0 ? 0 : int(stations.size()) - 1;
        SelectRadioBand(band, false);
        SetDialPosition(target);
    } else {
        GlideDial(target);
    }
    tune_index_ = radio_band_list_[band].stations[target];
    ScheduleTune();
}

void Tab5NativeApps::ScheduleTune() {
    tune_due_ms_ = lv_tick_get() + kTuneRestMs;
    if (!tune_due_ms_)
        tune_due_ms_ = 1;
}

void Tab5NativeApps::FollowCurrentStation() {
    // A requested song is not on the dial.
    if (!actions_.current_station || radio_song_)
        return;
    const int station = actions_.current_station();
    if (station < 0 || station >= int(radio_favorites_.size()))
        return;
    if (tune_sent_index_ >= 0) {
        if (station != tune_sent_index_ && lv_tick_get() - tune_sent_ms_ < 4000)
            return;
        tune_sent_index_ = -1;
    }
    radio_current_ = station;
    if (dial_dragging_ || tune_due_ms_)
        return;
    int at = BandPosition(station);
    if (at < 0) {
        // Changed by voice or the service: show it in its own band.
        const std::string name = actions_.station_band ? actions_.station_band(station) : "";
        for (size_t i = 1; i + 1 < radio_band_list_.size(); ++i)
            if (radio_band_list_[i].name == name) {
                SelectRadioBand(int(i), false);
                return;
            }
        SelectRadioBand(int(radio_band_list_.size()) - 1, false);
        return;
    }
    if (std::fabs(dial_pos_ - at) > 0.01f)
        GlideDial(at);
}

void Tab5NativeApps::RefreshTuner() {
    if (radio_band_ < 0 || !radio_dial_)
        return;
    const auto& stations = radio_band_list_[radio_band_].stations;
    const int at = int(std::lround(dial_pos_));
    const bool valid = at >= 0 && at < int(stations.size());
    auto name = [this](int station) {
        return actions_.station_name ? actions_.station_name(station) : std::string();
    };
    SetLabelTextIfChanged(radio_prev_name_, valid && at > 0 ? name(stations[at - 1]).c_str() : "");
    SetLabelTextIfChanged(radio_next_name_, valid && at + 1 < int(stations.size())
                                                ? name(stations[at + 1]).c_str()
                                                : "");
    char count[32];
    if (valid)
        std::snprintf(count, sizeof(count), "%d / %d", at + 1, int(stations.size()));
    else
        count[0] = 0;
    SetLabelTextIfChanged(radio_count_, count);
    if (!radio_song_ && valid) {
        const int station = stations[at];
        // While tuning, the title previews the station under the needle.
        if (dial_dragging_ || dial_gliding_ || tune_due_ms_ || station != radio_current_ ||
            current_station_name_.empty())
            SetLabelTextIfChanged(radio_station_, name(station).c_str());
        const std::string label = actions_.station_label  ? actions_.station_label(station)
                                  : actions_.station_band ? actions_.station_band(station)
                                                          : "";
        SetLabelTextIfChanged(radio_eyebrow_, label.c_str());
    }
    if (radio_fav_)
        lv_obj_invalidate(radio_fav_);
}

nabo_radio::Mood Tab5NativeApps::RadioMood() const {
    nabo_radio::Mood mood;
    mood.link = nabo_radio::LinkFromState(radio_link_state_.c_str());
    mood.music = radio_song_ || (actions_.station_band && radio_current_ >= 0 &&
                                 actions_.station_band(radio_current_) == "音乐");
    mood.tuning = dial_dragging_ || dial_gliding_ || tune_due_ms_ != 0;
    return mood;
}

void Tab5NativeApps::TickRadio() {
    const bool visible = IsRadioVisible();
    if (radio_stage_ && visible != radio_stage_active_) {
        radio_stage_active_ = visible;
        radio_stage_->SetActive(visible);
        if (!visible) {
            tune_due_ms_ = 0;
            tune_index_ = -1;
            dial_dragging_ = false;
        }
    }
    if (!visible)
        return;
    const uint32_t now = lv_tick_get();
    if (tune_due_ms_ && !dial_dragging_ && int32_t(now - tune_due_ms_) >= 0) {
        tune_due_ms_ = 0;
        const int station = tune_index_;
        tune_index_ = -1;
        const bool playing = radio_link_state_ == "Playing";
        if (station >= 0 && actions_.radio_select && (station != radio_current_ || !playing)) {
            radio_current_ = station;
            tune_sent_index_ = station;
            tune_sent_ms_ = now;
            SetLabelTextIfChanged(radio_state_, "连接中");
            Schedule([select = actions_.radio_select, station] { select(station); });
        }
    }
    if (radio_stage_) {
        const int level = actions_.radio_level ? actions_.radio_level() : 0;
        radio_stage_->Tick(now, RadioMood(), level);
    }
}

void Tab5NativeApps::SetRadioState(const char* station, const char* state, const char* meta) {
    // Must be called with the display lock held. Never Schedule LVGL work to
    // main without that lock: lv_inv_area asserts if invalidate runs during
    // rendering and wedges the main task (watchdog + dead play queue).
    const bool station_changed = station && *station && current_station_name_ != station;
    if (station && *station) {
        if (station_changed)
            current_station_name_ = station;
        // Always refresh the title so selecting a station cannot leave it blank
        // after a music track previously owned this label.
        if (!dial_dragging_ && !tune_due_ms_)
            SetLabelTextIfChanged(radio_station_, station);
    }
    if (meta)
        SetLabelTextIfChanged(radio_meta_, meta);
    radio_link_state_ = state ? state : "";
    const char* localized = "待播放";
    uint32_t color = kMuted;
    if (state) {
        if (std::strcmp(state, "Playing") == 0)
            localized = "播放中", color = kLevel;
        else if (std::strcmp(state, "Buffering") == 0)
            localized = "缓冲中", color = kAmber;
        else if (std::strcmp(state, "Connecting") == 0 || std::strcmp(state, "Reconnecting") == 0)
            localized = "连接中", color = kAmber;
        else if (std::strcmp(state, "Paused") == 0)
            localized = "已暂停";
        else if (std::strcmp(state, "Stopped") == 0)
            localized = "已停止";
        else if (std::strcmp(state, "Error") == 0)
            localized = "播放失败", color = kRed;
        else if (std::strcmp(state, "Waiting WiFi") == 0)
            localized = "等待网络", color = kAmber;
        else if (std::strcmp(state, "Unavailable") == 0)
            localized = "暂不可用", color = kRed;
    }
    SetLabelTextIfChanged(radio_state_, localized);
    if (color != radio_state_color_) {
        radio_state_color_ = color;
        lv_obj_set_style_text_color(radio_state_, lv_color_hex(color), 0);
    }
    radio_playing_ = state && std::strcmp(state, "Playing") == 0;
    radio_station_name_ = station ? station : "";
    if (podcast_page_)
        podcast_page_->SetPlayback(radio_playing_, radio_station_name_.c_str());
    const bool play_requested =
        actions_.radio_play_requested ? actions_.radio_play_requested() : radio_playing_;
    SetLabelTextIfChanged(radio_play_label_, play_requested ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    if (!radio_song_)
        SetMusicLyricLine(nullptr);
    if (station_changed ||
        (actions_.current_station && actions_.current_station() != radio_current_))
        FollowCurrentStation();
}

void Tab5NativeApps::OnDial(lv_event_t* event) {
    auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(event));
    if (!self || self->radio_band_ < 0)
        return;
    const auto& stations = self->radio_band_list_[self->radio_band_].stations;
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        lv_anim_delete(self, nullptr);
        self->dial_gliding_ = false;
        self->dial_dragging_ = true;
        self->dial_drag_from_ = self->dial_pos_;
        self->dial_drag_dx_ = 0;
        return;
    }
    if (!self->dial_dragging_)
        return;
    if (code == LV_EVENT_PRESSING) {
        lv_point_t vect;
        lv_indev_get_vect(lv_indev_active(), &vect);
        self->dial_drag_dx_ += vect.x;
        const float limit = std::max(0.0f, float(stations.size()) - 0.6f);
        self->SetDialPosition(
            std::clamp(self->dial_drag_from_ - self->dial_drag_dx_ / kStationGap, -0.4f, limit));
        return;
    }
    self->dial_dragging_ = false;
    if (stations.empty())
        return;
    if (std::abs(self->dial_drag_dx_) > 6) {
        const int target =
            std::clamp(int(std::lround(self->dial_pos_)), 0, int(stations.size()) - 1);
        self->GlideDial(target);
        self->tune_index_ = stations[target];
        self->ScheduleTune();
        return;
    }
    // A tap on either end of the dial steps one station that way.
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    lv_area_t area;
    lv_obj_get_coords(self->radio_dial_, &area);
    const int x = point.x - area.x1;
    if (x < kColW / 3)
        self->StepStation(-1);
    else if (x > kColW * 2 / 3)
        self->StepStation(1);
    else if (std::fabs(self->dial_pos_ - std::lround(self->dial_pos_)) > 0.01f)
        self->GlideDial(std::clamp(int(std::lround(self->dial_pos_)), 0, int(stations.size()) - 1));
}

void Tab5NativeApps::DrawDial(lv_event_t* event) {
    auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(event));
    auto* layer = lv_event_get_layer(event);
    if (!self || !layer || self->radio_band_ < 0)
        return;
    lv_area_t box;
    lv_obj_get_coords(self->radio_dial_, &box);
    const auto& band = self->radio_band_list_[self->radio_band_];
    const int n = int(band.stations.size());
    const float center = box.x1 + kColW / 2.0f;
    const float pos = self->dial_pos_;

    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_color = lv_color_hex(kDialLine);
    lv_area_t base{box.x1 + 12, box.y1 + 98, box.x2 - 12, box.y1 + 98};
    lv_draw_rect(layer, &rect, &base);

    lv_draw_label_dsc_t text;
    lv_draw_label_dsc_init(&text);
    text.font = &lv_font_montserrat_14;
    text.color = lv_color_hex(kAmberDim);
    text.align = LV_TEXT_ALIGN_CENTER;

    const int first = int(std::floor((box.x1 - center) / (kStationGap / 4) + pos * 4)) - 1;
    const int last = int(std::ceil((box.x2 - center) / (kStationGap / 4) + pos * 4)) + 1;
    for (int k = first; k <= last; ++k) {
        const int x = int(std::lround(center + (k / 4.0f - pos) * kStationGap));
        if (x < box.x1 + 8 || x > box.x2 - 8)
            continue;
        if (k % 4) {
            if (k > 0 && k < (n - 1) * 4) {
                rect.bg_color = lv_color_hex(kAmberDim);
                lv_area_t minor{x, box.y1 + 80, x, box.y1 + 97};
                lv_draw_rect(layer, &rect, &minor);
            }
            continue;
        }
        const int i = k / 4;
        if (i < 0 || i >= n)
            continue;
        const bool here = std::fabs(i - pos) < 0.5f;
        rect.bg_color = lv_color_hex(here ? 0xffd690 : kAmber);
        rect.radius = 0;
        lv_area_t major{x - 1, box.y1 + 58, x, box.y1 + 97};
        lv_draw_rect(layer, &rect, &major);
        rect.radius = LV_RADIUS_CIRCLE;
        lv_area_t dot{x - 4, box.y1 + 108, x + 4, box.y1 + 116};
        lv_draw_rect(layer, &rect, &dot);
        rect.radius = 0;
        const int station = band.stations[i];
        if (station < int(self->radio_favorites_.size()) && self->radio_favorites_[station])
            FillHeart(layer, x, box.y1 + 42, 5, kHeart);
        if (i == 0 || (i + 1) % 5 == 0) {
            char number[12];
            std::snprintf(number, sizeof(number), "%d", i + 1);
            text.text = number;
            text.text_local = 1;
            lv_area_t at{x - 20, box.y1 + 122, x + 20, box.y1 + 140};
            lv_draw_label(layer, &text, &at);
        }
    }
    text.font = &qd_font_cjk_28;
    text.align = LV_TEXT_ALIGN_LEFT;
    text.text = band.name.c_str();
    text.text_local = 1;
    lv_area_t title{box.x1 + 16, box.y1 + 6, box.x1 + 260, box.y1 + 46};
    lv_draw_label(layer, &text, &title);
    if (!n) {
        text.color = lv_color_hex(kMuted);
        text.align = LV_TEXT_ALIGN_CENTER;
        text.text = "这个波段还没有台";
        lv_area_t empty{box.x1, box.y1 + 56, box.x2, box.y1 + 96};
        lv_draw_label(layer, &text, &empty);
    }
}

void Tab5NativeApps::DrawHeart(lv_event_t* event) {
    auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(event));
    auto* layer = lv_event_get_layer(event);
    if (!self || !layer)
        return;
    bool favorite = false;
    if (self->radio_band_ >= 0) {
        const auto& stations = self->radio_band_list_[self->radio_band_].stations;
        const int at = int(std::lround(self->dial_pos_));
        if (at >= 0 && at < int(stations.size()) &&
            stations[at] < int(self->radio_favorites_.size()))
            favorite = self->radio_favorites_[stations[at]];
    }
    lv_area_t box;
    lv_obj_get_coords(self->radio_fav_, &box);
    FillHeart(layer, (box.x1 + box.x2) / 2, (box.y1 + box.y2) / 2 + 2, 10,
              favorite ? kHeart : kDimmer);
}
