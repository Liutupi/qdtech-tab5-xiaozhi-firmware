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

namespace {
bool SameMuseList(const tab5_muse::Snapshot& a, const tab5_muse::Snapshot& b) {
    if (a.latest_id != b.latest_id || a.seen_id != b.seen_id ||
        a.messages.size() != b.messages.size())
        return false;
    for (size_t i = 0; i < a.messages.size(); ++i) {
        const auto& left = a.messages[i];
        const auto& right = b.messages[i];
        if (left.id != right.id || left.title != right.title || left.body != right.body ||
            left.from != right.from || left.time != right.time)
            return false;
    }
    return true;
}

std::shared_ptr<LvglFont> MusicTextFontOwner() {
    auto* theme = LvglThemeManager::GetInstance().GetTheme("dark");
    if (theme) {
        auto font = theme->GetTextFont();
        if (font && font->font())
            return font;
    }
    return nullptr;
}

const lv_font_t* MusicTextFont() {
    auto owner = MusicTextFontOwner();
    return owner ? owner->font() : &qd_font_cjk_28;
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

void Tab5NativeApps::SetLabelTextIfChanged(lv_obj_t* label, const char* text) {
    if (!label)
        return;
    const char* next = text ? text : "";
    const char* current = lv_label_get_text(label);
    if (!current || std::strcmp(current, next) != 0)
        lv_label_set_text(label, next);
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
    muse_page_ = Card(root_, 0, 0, 1280, 720, 0x0d1b2b, 0x0d1b2b, 0);
    for (auto* page : {home_page_, settings_page_, radio_page_, game_page_, muse_page_})
        lv_obj_set_style_border_width(page, 0, 0);
    BuildHome();
    BuildSettings();
    BuildRadio();
    BuildGame();
    BuildMuse();
    lv_obj_add_flag(muse_page_, LV_OBJ_FLAG_HIDDEN);
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
    if (page && IsVisible() && !lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN)) {
        game_page_visible_.store(page == game_page_, std::memory_order_release);
        return;
    }
    game_page_visible_.store(false, std::memory_order_release);
    for (auto* candidate : {home_page_, settings_page_, radio_page_, game_page_, muse_page_,
                           icu_page_ ? icu_page_->object() : nullptr}) {
        if (!candidate) continue;
        lv_obj_add_flag(candidate, LV_OBJ_FLAG_HIDDEN);
    }
    if (ir_page_) ir_page_->Hide();
    if (mijia_page_) mijia_page_->Hide();
    if (podcast_page_) podcast_page_->Hide();
    if (page) {
        lv_obj_remove_flag(page, LV_OBJ_FLAG_HIDDEN);
        // Keep the panel clickable so taps don't fall through to the
        // workbench chat button underneath (that caused flash + dead UI).
        lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    game_page_visible_.store(page == game_page_, std::memory_order_release);
}

void Tab5NativeApps::OpenApps() { Show(home_page_); }

void Tab5NativeApps::OpenMuse() {
    // Opening the inbox marks the current batch read. Render that state once
    // before showing the page; the later persistence callback need not rebuild it.
    if (muse_snapshot_.seen_id < muse_snapshot_.latest_id) {
        muse_snapshot_.seen_id = muse_snapshot_.latest_id;
        muse_list_dirty_ = true;
        SetLabelTextIfChanged(muse_entry_label_, "打开推送");
    }
    if (muse_list_dirty_ || muse_rendered_font_ != MusicTextFont()) {
        RenderMuseList(muse_snapshot_);
        muse_list_dirty_ = false;
    }
    Show(muse_page_);
    Schedule(actions_.muse_refresh);
    Schedule(actions_.muse_opened);
}

void Tab5NativeApps::OpenSettings() {
    Show(settings_page_);
    RefreshSettings();
}

void Tab5NativeApps::OpenRadio(bool start_playback) {
    const bool was_visible = IsRadioVisible();
    // BuildRadio keeps its static fonts while hidden. The theme font can be replaced by
    // the SD font after startup, so bind the current owner before LVGL lays out this page.
    SyncRadioTextFont();
    Show(radio_page_);
    if (!was_visible)
        RefreshStations();
    if (start_playback)
        Schedule(actions_.start_radio);
}

void Tab5NativeApps::OpenNes() {
    Show(game_page_);
    ShowGameSelect();
    Schedule(actions_.start_nes);
    Schedule(actions_.nes_status);
}

void Tab5NativeApps::OpenHomeHub() {
    game_page_visible_.store(false, std::memory_order_release);
    if (!mijia_page_)
        mijia_page_ = std::make_unique<Tab5HomePage>(root_, [this] { OpenApps(); }, [this] { OpenIr(); });
    for (auto* candidate : {home_page_, settings_page_, radio_page_, game_page_, muse_page_,
                           icu_page_ ? icu_page_->object() : nullptr}) {
        if (!candidate) continue;
        lv_obj_add_flag(candidate, LV_OBJ_FLAG_HIDDEN);
    }
    if (podcast_page_) podcast_page_->Hide();
    if (ir_page_) ir_page_->Hide();
    mijia_page_->Show();
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
}

void Tab5NativeApps::SetHomeStatus(const tab5_home::Status& status) {
    if (mijia_page_ && mijia_page_->IsVisible())
        mijia_page_->SetStatus(status);
}

void Tab5NativeApps::OpenIr() {
    game_page_visible_.store(false, std::memory_order_release);
    if (!ir_page_)
        ir_page_ = std::make_unique<Tab5IrRemotePage>(root_, [this] { OpenHomeHub(); });
    for (auto* candidate : {home_page_, settings_page_, radio_page_, game_page_, muse_page_,
                           icu_page_ ? icu_page_->object() : nullptr}) {
        if (!candidate) continue;
        lv_obj_add_flag(candidate, LV_OBJ_FLAG_HIDDEN);
    }
    if (podcast_page_) podcast_page_->Hide();
    if (mijia_page_) mijia_page_->Hide();
    ir_page_->Show();
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
}

void Tab5NativeApps::OpenPodcast() {
    if (!podcast_page_) {
        Tab5PodcastPage::Callbacks callbacks;
        callbacks.back = [this] { OpenApps(); };
        callbacks.refresh = [this] { Schedule(actions_.muse_refresh); };
        callbacks.play_url = [this](const std::string& url, const std::string& title) {
            return actions_.podcast_play_url ? actions_.podcast_play_url(url, title) : false;
        };
        callbacks.play_track = [this](int episode, int index, int count) {
            return actions_.podcast_play ? actions_.podcast_play(episode, index, count) : false;
        };
        callbacks.stop = [this] { Schedule(actions_.radio_stop); };
        callbacks.level = [this] { return actions_.radio_level ? actions_.radio_level() : 0; };
        callbacks.playback_position = [this](std::string_view url) {
            return actions_.podcast_position ? actions_.podcast_position(url)
                                             : tab5_playback::Position{};
        };
        callbacks.text_font = [] { return MusicTextFontOwner(); };
        podcast_page_ = std::make_unique<Tab5PodcastPage>(root_, std::move(callbacks));
        podcast_page_->SetEpisodes(podcast_episodes_);
        const auto snapshot = tab5_muse::Inbox::GetInstance().Current();
        podcast_page_->SetRefreshResult(snapshot.podcast_ok, snapshot.poll_count);
    }
    Show(nullptr);
    podcast_page_->SetPlayback(radio_playing_, radio_station_name_.c_str());
    podcast_page_->Show();
    Schedule(actions_.muse_refresh);
}

void Tab5NativeApps::UpdatePodcastEntry() {
    if (!podcast_entry_detail_ || !podcast_entry_label_)
        return;
    if (!podcast_episodes_ || podcast_episodes_->empty()) {
        SetLabelTextIfChanged(podcast_entry_detail_, "每日音乐电台");
        SetLabelTextIfChanged(podcast_entry_label_, "等待今日节目");
        return;
    }
    const auto& latest = podcast_episodes_->front();
    const std::string title = tab5_podcast::DisplayText(latest.title);
    SetLabelTextIfChanged(podcast_entry_detail_, title.c_str());
    char text[64];
    std::snprintf(text, sizeof(text), "收听 · %u 首歌", unsigned(latest.tracks.size()));
    SetLabelTextIfChanged(podcast_entry_label_, text);
}

void Tab5NativeApps::Tick() {
    UpdateWave();
    if (podcast_page_ && IsVisible())
        podcast_page_->Tick();
    if (IsRadioVisible() && music_font_ != MusicTextFont())
        SyncRadioTextFont();
    if (IsVisible() && muse_page_ && !lv_obj_has_flag(muse_page_, LV_OBJ_FLAG_HIDDEN) &&
        muse_rendered_font_ != MusicTextFont()) {
        RenderMuseList(muse_snapshot_);
        muse_list_dirty_ = false;
    }
    // If the emulator could not publish its first fallback frame while LVGL
    // was busy, the next UI tick still makes the failure visible.
    if (game_fallback_error_.load() && IsVisible() && game_page_ &&
        !lv_obj_has_flag(game_page_, LV_OBJ_FLAG_HIDDEN))
        SetLabelTextIfChanged(game_status_, "画面内存不足，游戏已停止");
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
            // The list changes only when the scanner or selection changes.
            const int count = actions_.nes_rom_count ? actions_.nes_rom_count() : 0;
            const int selected = actions_.nes_rom_index ? actions_.nes_rom_index() : 0;
            if (count != game_list_count_ || selected != game_list_index_)
                RefreshGameRoms();
        }
        constexpr uint8_t kExitCombo = kNesBtnSelect | kNesBtnStart;
        const bool exit_pressed = playing ? ((pad & kExitCombo) == kExitCombo && (pressed & kExitCombo))
                                          : (pressed & kNesBtnSelect) != 0;
        if (exit_pressed)
            ExitGame(playing);
    }
}

void Tab5NativeApps::OpenIcu(int mode, const std::string& external_result, bool result_ok) {
    if (!icu_page_) {
        icu_page_ = std::make_unique<Tab5IcuPage>(root_, [this] { OpenApps(); });
        // Start hidden like every other page: Show() skips pages that already look visible,
        // and a visible new page left the app home shown underneath, so 返回应用 did nothing.
        lv_obj_add_flag(icu_page_->object(), LV_OBJ_FLAG_HIDDEN);
    }
    Show(icu_page_->object());
    icu_page_->Open(mode, external_result, result_ok);
}

void Tab5NativeApps::Close() {
    game_page_visible_.store(false, std::memory_order_release);
    if (mijia_page_) mijia_page_->Hide();  // stops the fast device poll
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

bool Tab5NativeApps::IsVisible() const {
    return root_ && !lv_obj_has_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

bool Tab5NativeApps::IsRadioVisible() const {
    return IsVisible() && !lv_obj_has_flag(radio_page_, LV_OBJ_FLAG_HIDDEN);
}

bool Tab5NativeApps::IsPodcastVisible() const {
    return IsVisible() && podcast_page_ && podcast_page_->IsVisible();
}

void Tab5NativeApps::SetPodcastNowPlaying(const char* title, const char* artist,
                                          const char* line) {
    if (podcast_page_)
        podcast_page_->SetNowPlaying(title, artist, line);
}

bool Tab5NativeApps::IsSettingsVisible() const {
    return IsVisible() && !lv_obj_has_flag(settings_page_, LV_OBJ_FLAG_HIDDEN);
}

void Tab5NativeApps::BuildHome() {
    Label(home_page_, "土皮助手", &qd_font_lxgw_36, 0xf5f9fd, 52, 27, 450);
    Label(home_page_, "应用与设置", &qd_font_lxgw_28, 0x9bb7ca, 54, 76, 500);
    Button(
        home_page_, "返回 Nabo", 1016, 34, 216, 60,
        [](lv_event_t* event) {
            static_cast<Tab5NativeApps*>(lv_event_get_user_data(event))->Close();
        },
        this);

    // The six destinations share one large touch target. Keep every label inside
    // the card and let taps on text bubble to the card's click handler.
    auto entry = [this](int x, int y, uint32_t accent, const char* title, const char* detail,
                        const char* action, lv_event_cb_t callback,
                        lv_obj_t** detail_out = nullptr) -> lv_obj_t* {
        auto* card = Card(home_page_, x, y, 554, 174, 0x142d43, 0x35627d, 26);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x234761), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(card, lv_color_hex(accent), LV_STATE_PRESSED);
        auto* accent_bar = Card(card, 24, 21, 7, 44, accent, accent, 3);
        lv_obj_remove_flag(accent_bar, LV_OBJ_FLAG_CLICKABLE);
        auto* heading = Label(card, title, &qd_font_lxgw_36, 0xf5f9fd, 51, 15, 470);
        auto* description = Label(card, detail, &qd_font_lxgw_28, 0xa8c4d3, 52, 69, 470);
        auto* affordance = Label(card, action, &qd_font_lxgw_28, accent, 52, 122, 470);
        for (auto* label : {heading, description, affordance}) {
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
            lv_obj_set_width(label, 470);
            lv_obj_set_height(label, 42);
            lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);
        }
        lv_obj_add_event_cb(card, callback, LV_EVENT_CLICKED, this);
        if (detail_out)
            *detail_out = description;
        return affordance;
    };

    // Settings stay one tap away from the Nabo home ("设置" button); this slot now
    // holds the NABO 电台 (Muse daily music radio).
    podcast_entry_label_ = entry(
        54, 140, 0xf59ab5, "NABO 电台", "每日音乐电台", "等待今日节目",
        [](lv_event_t* event) {
            static_cast<Tab5NativeApps*>(lv_event_get_user_data(event))->OpenPodcast();
        },
        &podcast_entry_detail_);
    // Episode titles are arbitrary Chinese: use the full CJK font for the detail line.
    lv_obj_set_style_text_font(podcast_entry_detail_, &qd_font_cjk_28, 0);
    UpdatePodcastEntry();
    entry(672, 140, 0xf7c84d, "网络电台", "选台 · 播放 · 切换", "打开电台", [](lv_event_t* event) {
        static_cast<Tab5NativeApps*>(lv_event_get_user_data(event))->OpenRadio();
    });
    entry(54, 326, 0x89c8a7, "ICU 数值工具", "肾功能 · 氧合 · 血气 · 静脉泵", "打开计算",
          [](lv_event_t* event) {
              static_cast<Tab5NativeApps*>(lv_event_get_user_data(event))->OpenIcu();
          });
    entry(672, 326, 0xe87c64, "NES", "红白机 · USB 控制 · SD 卡 ROM", "进入游戏",
          [](lv_event_t* event) {
              static_cast<Tab5NativeApps*>(lv_event_get_user_data(event))->OpenNes();
          });
    entry(54, 512, 0x86a8e8, "米家中控", "场景 · 灯 · 空调 · 红外遥控", "打开中控", [](lv_event_t* event) {
        static_cast<Tab5NativeApps*>(lv_event_get_user_data(event))->OpenHomeHub();
    });
    muse_entry_label_ =
        entry(672, 512, 0xa99bff, "NABO 每日推送", "Muse 整理 · NAS 推送", "打开推送", [](lv_event_t* event) {
            static_cast<Tab5NativeApps*>(lv_event_get_user_data(event))->OpenMuse();
        });
}

void Tab5NativeApps::BuildMuse() {
    Label(muse_page_, "NABO 每日推送", &qd_font_lxgw_28, 0xf5f9fd, 52, 30, 440);
    muse_status_ = Label(muse_page_, "正在连接 NAS…", &qd_font_cjk_28, 0x9bb7ca, 54, 76, 760);
    lv_label_set_long_mode(muse_status_, LV_LABEL_LONG_DOT);
    lv_obj_set_height(muse_status_, 34);
    Button(muse_page_, "刷新", 800, 34, 180, 60, [](lv_event_t* event) {
        auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(event));
        lv_label_set_text(self->muse_status_, "正在刷新…");
        Schedule(self->actions_.muse_refresh);
    }, this);
    Button(muse_page_, "返回应用", 1016, 34, 216, 60, [](lv_event_t* event) {
        static_cast<Tab5NativeApps*>(lv_event_get_user_data(event))->OpenApps();
    }, this);

    muse_list_ = lv_obj_create(muse_page_);
    lv_obj_set_pos(muse_list_, 54, 128);
    lv_obj_set_size(muse_list_, 1172, 530);
    lv_obj_set_style_bg_opa(muse_list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(muse_list_, 0, 0);
    lv_obj_set_style_pad_all(muse_list_, 0, 0);
    lv_obj_set_style_pad_row(muse_list_, 14, 0);
    lv_obj_set_flex_flow(muse_list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(muse_list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(muse_list_, LV_SCROLLBAR_MODE_AUTO);

    muse_url_ = Label(muse_page_, "", &qd_font_cjk_28, 0x7f8fa3, 54, 670, 1172);
    lv_label_set_long_mode(muse_url_, LV_LABEL_LONG_DOT);
    lv_obj_set_height(muse_url_, 34);

    RenderMuseList(muse_snapshot_);
}

void Tab5NativeApps::RenderMuseList(const tab5_muse::Snapshot& snapshot) {
    // LVGL styles hold raw font pointers. Keep the old owner through lv_obj_clean,
    // then pin the new owner until this list is rebuilt or destroyed.
    auto previous_font_owner = std::move(muse_font_owner_);
    muse_font_owner_ = MusicTextFontOwner();
    muse_rendered_font_ = muse_font_owner_ ? muse_font_owner_->font() : &qd_font_cjk_28;
    lv_obj_clean(muse_list_);
    if (snapshot.messages.empty()) {
        auto* hint = Card(muse_list_, 0, 0, 1150, 190, 0x142d43, 0x35627d, 24);
        lv_obj_remove_flag(hint, LV_OBJ_FLAG_SCROLLABLE);
        Label(hint, "还没有推送", &qd_font_cjk_28, 0xf5f9fd, 30, 26, 1080);
        auto* text = Label(hint,
                           "在 Muse 里添加本 NAS 的 MCP 地址 (见页面底部或 NAS 容器日志)，"
                           "然后让 Muse 用 tab5_push 把整理好的内容推送过来。",
                           &qd_font_cjk_28, 0xa8c4d3, 30, 80, 1080);
        lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
        return;
    }
    for (const auto& m : snapshot.messages) {
        const bool unread = m.id > snapshot.seen_id;
        auto* card = lv_obj_create(muse_list_);
        lv_obj_set_width(card, 1150);
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, lv_color_hex(unread ? 0x1f2a4d : 0x142d43), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(unread ? 0x8a7cf0 : 0x35627d), 0);
        lv_obj_set_style_border_width(card, unread ? 2 : 1, 0);
        lv_obj_set_style_radius(card, 22, 0);
        lv_obj_set_style_pad_all(card, 22, 0);
        lv_obj_set_style_pad_row(card, 8, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        auto* title = lv_label_create(card);
        lv_label_set_text(title, m.title.c_str());
        lv_obj_set_width(title, 1100);
        lv_obj_set_style_text_font(title, muse_rendered_font_, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(0xf5f9fd), 0);

        std::string meta = m.time;
        if (!m.from.empty())
            meta += "  ·  " + m.from;
        if (unread)
            meta += "  ·  新";
        auto* meta_label = lv_label_create(card);
        lv_label_set_text(meta_label, meta.c_str());
        lv_obj_set_style_text_font(meta_label, &qd_font_cjk_28, 0);
        lv_obj_set_style_text_color(meta_label, lv_color_hex(unread ? 0xb9adff : 0x7f9bb0), 0);

        auto* body = lv_label_create(card);
        lv_label_set_text(body, m.body.c_str());
        lv_obj_set_width(body, 1100);
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(body, muse_rendered_font_, 0);
        lv_obj_set_style_text_color(body, lv_color_hex(0xd6e4ee), 0);
        lv_obj_set_style_text_line_space(body, 6, 0);
    }
    lv_obj_scroll_to_y(muse_list_, 0, LV_ANIM_OFF);
}

void Tab5NativeApps::SetMuseInbox(const tab5_muse::Snapshot& snapshot) {
    podcast_episodes_ = snapshot.episodes;
    UpdatePodcastEntry();
    if (podcast_page_) {
        podcast_page_->SetEpisodes(podcast_episodes_);
        podcast_page_->SetRefreshResult(snapshot.podcast_ok, snapshot.poll_count);
    }
    const int unread = snapshot.Unread();
    if (muse_entry_label_) {
        char text[64];
        if (unread > 0)
            std::snprintf(text, sizeof(text), "打开推送 · %d 条新消息", unread);
        else
            std::snprintf(text, sizeof(text), "打开推送");
        SetLabelTextIfChanged(muse_entry_label_, text);
    }
    if (muse_status_) {
        char text[160];
        if (!snapshot.ok && !snapshot.ever_ok)
            std::snprintf(text, sizeof(text), "%s",
                          snapshot.url.empty() ? "还没有设置 Muse 中转站地址"
                                               : "无法连接 Muse 中转站，稍后自动重试");
        else if (!snapshot.ok)
            std::snprintf(text, sizeof(text), "NAS 暂时无响应，显示的是上次内容");
        else
            std::snprintf(text, sizeof(text), "共 %u 条 · 未读 %d · 每分钟自动刷新",
                          unsigned(snapshot.messages.size()), unread);
        SetLabelTextIfChanged(muse_status_, text);
    }
    if (muse_url_) {
        const std::string url = snapshot.mcp_url.empty()
                                    ? std::string("Muse 接入地址：公网隧道未就绪")
                                    : "Muse 接入地址：" + snapshot.mcp_url;
        SetLabelTextIfChanged(muse_url_, url.c_str());
    }
    // Keep the newest data while the inbox is hidden. Building up to 12 cards
    // here would otherwise block the LVGL task for a page nobody can see.
    if (!SameMuseList(snapshot, muse_snapshot_)) {
        muse_snapshot_ = snapshot;
        muse_list_dirty_ = true;
    }
    if (muse_list_dirty_ && IsVisible() && !lv_obj_has_flag(muse_page_, LV_OBJ_FLAG_HIDDEN)) {
        RenderMuseList(muse_snapshot_);
        muse_list_dirty_ = false;
    }
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
    lv_obj_set_pos(brightness_slider_, 42, 124);
    lv_obj_set_size(brightness_slider_, 468, 48);
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
    lv_obj_set_pos(volume_slider_, 42, 124);
    lv_obj_set_size(volume_slider_, 494, 48);
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
        // The full theme font has a 43 px line height; keep its glyphs
        // inside the 48 px touch row instead of clipping them at 34 px.
        lv_obj_set_pos(station_names_[i], 15, 2);
        lv_obj_set_size(station_names_[i], 302, 44);
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
    Label(player, "NOW PLAYING", &qd_font_lxgw_28, 0x83d6e8, 38, 20, 220);
    // Voice status pill (聆听中 / 回应中 / last utterance) left of the 点歌 button.
    radio_voice_ = Label(player, "", &qd_font_cjk_28, 0x9ff0b8, 262, 22, 326);
    lv_obj_set_height(radio_voice_, 34);
    lv_label_set_long_mode(radio_voice_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(radio_voice_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_flag(radio_voice_, LV_OBJ_FLAG_HIDDEN);
    auto* ask_song = Button(
        player, "点歌", 600, 12, 156, 50,
        [](lv_event_t* event) {
            auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(event));
            if (!self->actions_.ask_song)
                return;
            lv_label_set_text(self->radio_state_, "请说出想听的歌名…");
            self->SetVoiceStatus("正在连接 Nabo…", true);
            Schedule(self->actions_.ask_song);
        },
        this);
    lv_obj_set_style_bg_color(ask_song, lv_color_hex(0x7a4fd6), 0);
    ask_song_label_ = lv_obj_get_child(ask_song, 0);
    // Use bundled CJK until the full theme font is ready for dynamic titles.
    radio_station_ = Label(player, "选择一个电台", &qd_font_cjk_28, 0xf5f9fd, 38, 68, 708);
    // Noto's 43 px line height needs 86 px for two lines of song title.
    lv_obj_set_height(radio_station_, 90);
    lv_label_set_long_mode(radio_station_, LV_LABEL_LONG_DOT);
    radio_state_ = Label(player, "待播放", &qd_font_lxgw_28, 0xf7c84d, 38, 162, 700);
    auto* wave_panel = Card(player, 28, 199, 728, 80, 0x0c2133, 0x346078, 22);
    radio_wave_ = lv_obj_create(wave_panel);
    lv_obj_set_pos(radio_wave_, 18, 6);
    lv_obj_set_size(radio_wave_, 692, 68);
    lv_obj_set_style_bg_opa(radio_wave_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(radio_wave_, 0, 0);
    lv_obj_set_style_pad_all(radio_wave_, 0, 0);
    lv_obj_clear_flag(radio_wave_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(radio_wave_, DrawWave, LV_EVENT_DRAW_MAIN, this);
    for (int i = 0; i < kWaveBars; ++i) {
        wave_heights_[i] = 5;
        wave_colors_[i] = lv_color_hsv_to_rgb((205 + i * 330 / kWaveBars) % 360, 78, 95);
    }
    radio_meta_ = Label(player, "", &qd_font_lxgw_28, 0x9bb7ca, 38, 283, 700);
    lv_obj_set_height(radio_meta_, 34);
    lv_label_set_long_mode(radio_meta_, LV_LABEL_LONG_DOT);
    auto* lyric_panel = Card(player, 28, 322, 728, 121, 0x0c2133, 0x346078, 16);
    // Show the current lyric in up to two centered lines, without scrolling.
    radio_lyric_previous_ = nullptr;
    radio_lyric_next_ = nullptr;
    radio_lyric_ = Label(lyric_panel, "等待歌词", &qd_font_cjk_28, 0xf5f9fd, 18, 36, 692);
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
        const int target = radio_playing_ ? std::clamp(10 + level * (20 + ripple) / 45, 6, 64) : 5;
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

    constexpr int kW = 960, kH = 720;
    // Create only the small LVGL object at boot. A normal Tab5 uses the two
    // panel framebuffers directly and never needs the 4 MiB fallback frames.
    game_canvas_ = lv_image_create(stage);
    if (game_canvas_) {
        lv_obj_set_pos(game_canvas_, (1280 - kW) / 2, 0);
        lv_obj_set_size(game_canvas_, kW, kH);
        lv_obj_add_flag(game_canvas_, LV_OBJ_FLAG_HIDDEN);
    }

    game_status_ = Label(stage, "手柄：↑↓ 选 ROM · ○ 开始 · Select 返回",
                         &qd_font_lxgw_28, 0x7f9aad, 24, 16, 900);
    game_rom_label_ = Label(stage, "", &qd_font_lxgw_36, 0xf5f9fd, 24, 64, 1080);
    lv_label_set_long_mode(game_rom_label_, LV_LABEL_LONG_DOT);
    // Touch exit in the right bar: the emulator only writes the centre 960 px, so it stays
    // visible while a game runs.
    auto* exit = Button(stage, "返回应用", 1128, 16, 136, 64, [](lv_event_t* event) {
        auto* self = static_cast<Tab5NativeApps*>(lv_event_get_user_data(event));
        self->ExitGame(self->game_playing_ui_.load());
    }, this);
    game_exit_label_ = lv_obj_get_child(exit, 0);

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

bool Tab5NativeApps::EnsureGameFallbackBuffers() {
    if (game_scaled_[0] && game_scaled_[1] && game_scaled_[2])
        return true;
    if (!game_canvas_)
        return false;

    constexpr int kW = 960, kH = 720;
    constexpr size_t kFrameBytes = size_t(kW) * kH * sizeof(uint16_t);
    uint16_t* allocated[3] = {};
    for (auto& frame : allocated) {
        frame = static_cast<uint16_t*>(
            heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!frame) {
            for (auto* previous : allocated)
                if (previous)
                    heap_caps_free(previous);
            return false;
        }
    }
    for (int i = 0; i < 3; ++i)
        game_scaled_[i] = allocated[i];
    game_write_idx_ = 0;
    game_img_.header.magic = LV_IMAGE_HEADER_MAGIC;
    game_img_.header.cf = LV_COLOR_FORMAT_RGB565;
    game_img_.header.w = kW;
    game_img_.header.h = kH;
    game_img_.header.stride = kW * sizeof(uint16_t);
    game_img_.data_size = kFrameBytes;
    ESP_LOGI("Tab5NativeApps", "NES LVGL fallback allocated %u bytes",
             unsigned(3 * kFrameBytes));
    return true;
}

void Tab5NativeApps::FailGameFallback() {
    if (game_fallback_error_.exchange(true))
        return;
    game_playing_ui_.store(false);
    ESP_LOGE("Tab5NativeApps", "NES LVGL fallback unavailable: PSRAM free=%u largest=%u",
             unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    if (lvgl_port_lock(100)) {
        ShowGameSelect();
        lvgl_port_unlock();
    }
    Schedule(actions_.stop_nes);
}

// Select (gamepad) or the on-screen button: a running game goes back to the ROM list,
// the ROM list goes back to the app home.
void Tab5NativeApps::ExitGame(bool playing) {
    if (playing) {
        // Drop further emu frames first so the list is not painted
        // under a still-running scaler, then ask the emu to stop.
        game_playing_ui_.store(false);
        game_stop_ui_pending_.store(true);
        Schedule(actions_.stop_nes);
        ShowGameSelect();
        game_stop_ui_pending_.store(false);
        return;
    }
    Schedule(actions_.stop_nes);
    Schedule([] {
        // Leave game mode entirely when exiting to the app home.
        Application::GetInstance().SetExternalAudioActive(false);
    });
    OpenApps();
}

void Tab5NativeApps::ShowGameSelect() {
    game_playing_ui_.store(false);
    if (game_exit_label_)
        lv_label_set_text(game_exit_label_, "返回应用");
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
    if (game_canvas_) {
        lv_obj_add_flag(game_canvas_, LV_OBJ_FLAG_HIDDEN);
    }
    RefreshGameRoms();
    if (game_fallback_error_.load())
        SetLabelTextIfChanged(game_status_, "画面内存不足，游戏已停止");
}

void Tab5NativeApps::ShowGamePlay() {
    game_playing_ui_.store(true);
    if (game_exit_label_)
        lv_label_set_text(game_exit_label_, "退出游戏");
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
    game_list_count_ = count;
    game_list_index_ = sel;
    if (count <= 0) {
        SetLabelTextIfChanged(game_rom_label_, "");
        for (auto* row : game_rom_rows_)
            if (row)
                lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        if (game_status_) {
            SetLabelTextIfChanged(game_status_, "SD /nes 未找到 ROM");
        }
        return;
    }
    if (game_rom_label_ && actions_.nes_rom_name)
        SetLabelTextIfChanged(game_rom_label_,
                              actions_.nes_rom_name(std::clamp(sel, 0, count - 1)).c_str());
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
            SetLabelTextIfChanged(game_rom_names_[i], text);
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
    if (!pixels || tab5_nes_video::Active()) return;
    if (width != 256 || height != 240) return;
    if (!game_page_visible_.load(std::memory_order_acquire) ||
        !game_playing_ui_.load() || game_fallback_error_.load()) return;
    // The direct panel path never reaches this callback. Allocate only after
    // its indexed-frame presenter has actually fallen back to RGB565 frames.
    if (!EnsureGameFallbackBuffers()) {
        FailGameFallback();
        return;
    }
    if (!game_page_visible_.load(std::memory_order_acquire) || !game_playing_ui_.load()) return;

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

    // Publish only after taking the LVGL lock. A missed lock drops this frame
    // without recycling the buffer LVGL is still displaying.
    const int ready = game_write_idx_;
    if (lvgl_port_lock(2)) {
        if (game_playing_ui_.load() && IsVisible() &&
            !lv_obj_has_flag(game_page_, LV_OBJ_FLAG_HIDDEN)) {
            game_img_.data = reinterpret_cast<const uint8_t*>(game_scaled_[ready]);
            lv_image_set_src(game_canvas_, &game_img_);
            if (lv_obj_has_flag(game_canvas_, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_remove_flag(game_canvas_, LV_OBJ_FLAG_HIDDEN);
                if (game_status_) {
                    lv_obj_set_pos(game_status_, 24, 16);
                    lv_obj_set_width(game_status_, 900);
                    SetLabelTextIfChanged(game_status_, "游戏中 · Select+Start 返回");
                }
            }
            lv_obj_invalidate(game_canvas_);
            game_write_idx_ = (ready + 1) % 3;
        }
        lvgl_port_unlock();
    }
}

void Tab5NativeApps::SetNesStatus(const char* status) {
    if (!game_status_) return;
    if (game_fallback_error_.load()) return;
    // Keep the in-game hint; "Loading/Playing" states arrive right after it.
    if (game_playing_ui_.load() && tab5_nes_video::Available()) return;
    SetLabelTextIfChanged(game_status_, status && *status ? status : "—");
}

void Tab5NativeApps::SetNesPlaying(bool playing) {
    if (playing) {
        game_fallback_error_.store(false);
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
    if (!radio_lyric_)
        return;
    const char* text = line && *line ? line : "等待歌词";
    SetLabelTextIfChanged(radio_lyric_, text);
    lv_point_t size{};
    lv_text_get_size(&size, text, lv_obj_get_style_text_font(radio_lyric_, LV_PART_MAIN),
                     lv_obj_get_style_text_letter_space(radio_lyric_, LV_PART_MAIN),
                     lv_obj_get_style_text_line_space(radio_lyric_, LV_PART_MAIN), 692,
                     LV_TEXT_FLAG_NONE);
    const int height = std::clamp<int>(size.y, 48, 90);
    lv_obj_set_height(radio_lyric_, height);
    lv_obj_set_y(radio_lyric_, (121 - height) / 2);
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
    if (radio_station_)
        lv_obj_set_style_text_font(radio_station_, font, 0);
    for (auto* lyric : {radio_lyric_previous_, radio_lyric_, radio_lyric_next_}) {
        if (lyric)
            lv_obj_set_style_text_font(lyric, font, 0);
    }
    for (auto* station_name : station_names_) {
        if (station_name)
            lv_obj_set_style_text_font(station_name, font, 0);
    }
    if (radio_lyric_)
        SetMusicLyricLine(lv_label_get_text(radio_lyric_));
}

void Tab5NativeApps::SetMusicLyricsWindow(const char* title, const char* artist,
                                          const char* previous, const char* current,
                                          const char* next) {
    if (IsRadioVisible())
        SyncRadioTextFont();
    if (title && *title) {
        std::string head(title);
        if (artist && *artist) {
            head += " - ";
            head += artist;
        }
        SetLabelTextIfChanged(radio_station_, head.c_str());
        SetLabelTextIfChanged(radio_next_label_, "下一首");
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
    SetLabelTextIfChanged(radio_next_label_, "下一台");
    SetLabelTextIfChanged(radio_lyric_previous_, "");
    SetMusicLyricLine(nullptr);
    SetLabelTextIfChanged(radio_lyric_next_, "");
}

void Tab5NativeApps::RefreshWifi() {
    if (actions_.wifi_summary)
        SetLabelTextIfChanged(wifi_label_, actions_.wifi_summary().c_str());
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
        SetLabelTextIfChanged(station_names_[row], text);
        const bool selected = name == current_station_name_;
        lv_obj_set_style_bg_color(station_rows_[row],
                                  lv_color_hex(selected ? 0x397b87 : 0x254c66), 0);
        lv_obj_set_style_border_color(station_rows_[row],
                                      lv_color_hex(selected ? 0x91dfde : 0x3f7590), 0);
    }
    char page[32];
    std::snprintf(page, sizeof(page), "%d / %d", station_page_ + 1, pages);
    SetLabelTextIfChanged(station_page_label_, page);
}

void Tab5NativeApps::SetRadioState(const char* station, const char* state,
                                   const char* meta) {
    // Must be called with the display lock held. Never Schedule LVGL work to
    // main without that lock: lv_inv_area asserts if invalidate runs during
    // rendering and wedges the main task (watchdog + dead play queue).
    const bool station_changed = station && *station && current_station_name_ != station;
    if (station && *station) {
        if (station_changed)
            current_station_name_ = station;
        // Always refresh the title so selecting a station cannot leave it blank
        // after a music track previously owned this label.
        SetLabelTextIfChanged(radio_station_, station);
    }
    if (meta)
        SetLabelTextIfChanged(radio_meta_, meta);
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
    SetLabelTextIfChanged(radio_state_, localized);
    radio_playing_ = state && std::strcmp(state, "Playing") == 0;
    radio_station_name_ = station ? station : "";
    if (podcast_page_)
        podcast_page_->SetPlayback(radio_playing_, radio_station_name_.c_str());
    const bool play_requested = actions_.radio_play_requested
                                    ? actions_.radio_play_requested()
                                    : radio_playing_;
    SetLabelTextIfChanged(radio_play_label_, play_requested ? "暂停" : "播放");
    // Row selection changes only when the station changes.
    if (station_changed)
        RefreshStations();
}
