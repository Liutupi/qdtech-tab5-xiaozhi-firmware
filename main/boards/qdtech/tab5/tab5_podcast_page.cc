#include "tab5_podcast_page.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "display/lvgl_display/lvgl_font.h"

LV_FONT_DECLARE(qd_font_lxgw_28);
LV_FONT_DECLARE(qd_font_lxgw_36);
LV_FONT_DECLARE(qd_font_cjk_28);

namespace {
constexpr uint32_t kPage = 0x0d1b2b, kPanel = 0x142d43, kPanelBorder = 0x35627d;
constexpr uint32_t kViolet = 0xa99bff, kPink = 0xf59ab5, kText = 0xf5f9fd, kMuted = 0x9bb7ca;
constexpr uint32_t kBody = 0xd6e4ee;

lv_obj_t* Box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t fill, uint32_t border,
              int radius) {
    auto* o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(o, border == fill ? 0 : 1, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t* Text(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color, int x,
               int y, int w) {
    auto* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

lv_obj_t* ScrollColumn(lv_obj_t* parent, int x, int y, int w, int h, int gap) {
    auto* o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_pad_row(o, gap, 0);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(o, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_AUTO);
    return o;
}
}  // namespace

Tab5PodcastPage::Tab5PodcastPage(lv_obj_t* parent, Callbacks callbacks)
    : callbacks_(std::move(callbacks)) {
    root_ = Box(parent, 0, 0, 1280, 720, kPage, kPage, 0);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < kBars; ++i) {
        heights_[i] = 6;
        // Violet → pink across the panel, matching the card accent.
        colors_[i] = lv_color_mix(lv_color_hex(kPink), lv_color_hex(kViolet), i * 255 / (kBars - 1));
    }
    Build();
}

void Tab5PodcastPage::Build() {
    Text(root_, "Muse 音乐电台", &qd_font_lxgw_36, kText, 52, 27, 600);
    summary_ = Text(root_, "每日一期 · 来自 Muse", &qd_font_cjk_28, kMuted, 54, 76, 900);
    lv_label_set_long_mode(summary_, LV_LABEL_LONG_DOT);
    lv_obj_set_height(summary_, 36);
    auto* back = lv_button_create(root_);
    lv_obj_set_pos(back, 1016, 34);
    lv_obj_set_size(back, 216, 60);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x254c66), 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x39718b), LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, 18, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    auto* back_label = Text(back, "返回应用", &qd_font_lxgw_28, kText, 0, 0, 216);
    lv_obj_center(back_label);
    lv_obj_set_style_text_align(back_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_event_cb(
        back,
        [](lv_event_t* e) {
            auto* self = static_cast<Tab5PodcastPage*>(lv_event_get_user_data(e));
            if (self->callbacks_.back)
                self->callbacks_.back();
        },
        LV_EVENT_CLICKED, this);

    // Episode list.
    auto* left = Box(root_, 48, 128, 360, 560, kPanel, kPanelBorder, 27);
    Text(left, "往期节目", &qd_font_cjk_28, kMuted, 24, 18, 300);
    list_ = ScrollColumn(left, 14, 64, 332, 482, 10);

    // Player card: soft violet-to-navy gradient.
    auto* card = Box(root_, 428, 128, 804, 560, 0x1d1f4a, 0x5a4fb0, 27);
    lv_obj_set_style_bg_grad_color(card, lv_color_hex(kPanel), 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);

    badge_ = Box(card, 32, 24, 148, 38, 0x3a2f78, 0x3a2f78, 19);
    badge_label_ = Text(badge_, "本期节目", &qd_font_cjk_28, 0xd9d2ff, 0, 0, 148);
    lv_obj_set_style_text_align(badge_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(badge_label_, LV_ALIGN_CENTER, 0, 0);

    title_ = Text(card, "还没有节目", &qd_font_cjk_28, kText, 32, 74, 540);
    lv_label_set_long_mode(title_, LV_LABEL_LONG_DOT);
    lv_obj_set_height(title_, 46);
    meta_ = Text(card, "", &qd_font_cjk_28, kMuted, 32, 120, 540);
    lv_label_set_long_mode(meta_, LV_LABEL_LONG_DOT);
    lv_obj_set_height(meta_, 36);

    auto* play = lv_button_create(card);
    lv_obj_set_pos(play, 592, 40);
    lv_obj_set_size(play, 180, 84);
    lv_obj_set_style_radius(play, 42, 0);
    lv_obj_set_style_bg_color(play, lv_color_hex(kPink), 0);
    lv_obj_set_style_bg_grad_color(play, lv_color_hex(kViolet), 0);
    lv_obj_set_style_bg_grad_dir(play, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_color(play, lv_color_hex(0xd77d9a), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(play, 0, 0);
    play_label_ = Text(play, "播放本期", &qd_font_lxgw_28, 0x1a1033, 0, 0, 180);
    lv_obj_set_style_text_align(play_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(play_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(
        play,
        [](lv_event_t* e) { static_cast<Tab5PodcastPage*>(lv_event_get_user_data(e))->PlaySelected(); },
        LV_EVENT_CLICKED, this);

    auto* wave_panel = Box(card, 32, 166, 740, 104, 0x0c1530, 0x2f2a66, 22);
    wave_ = lv_obj_create(wave_panel);
    lv_obj_set_pos(wave_, 14, 8);
    lv_obj_set_size(wave_, 712, 88);
    lv_obj_set_style_bg_opa(wave_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wave_, 0, 0);
    lv_obj_set_style_pad_all(wave_, 0, 0);
    lv_obj_remove_flag(wave_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(wave_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wave_, DrawWave, LV_EVENT_DRAW_MAIN, this);

    Text(card, "节目文案", &qd_font_lxgw_28, kViolet, 32, 286, 300);
    script_box_ = ScrollColumn(card, 32, 326, 436, 214, 0);
    script_ = Text(script_box_, "", &qd_font_cjk_28, kBody, 0, 0, 420);
    lv_obj_set_style_text_line_space(script_, 8, 0);

    Text(card, "歌单 · 点按播放", &qd_font_cjk_28, kPink, 492, 286, 280);
    tracks_ = ScrollColumn(card, 488, 326, 288, 214, 6);

    status_ = Text(root_, "", &qd_font_cjk_28, kMuted, 428, 690, 804);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_DOT);
    lv_obj_set_height(status_, 30);
    RenderList();
}

void Tab5PodcastPage::Show() {
    BindFont();
    if (list_dirty_)
        RenderList();
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
}

void Tab5PodcastPage::Hide() { lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN); }

bool Tab5PodcastPage::IsVisible() const { return !lv_obj_has_flag(root_, LV_OBJ_FLAG_HIDDEN); }

// The full theme font may be replaced by the SD font after boot. LVGL styles hold raw
// pointers, so pin the owner while any label uses it and re-render on change.
void Tab5PodcastPage::BindFont() {
    auto owner = callbacks_.text_font ? callbacks_.text_font() : nullptr;
    const lv_font_t* font = owner && owner->font() ? owner->font() : &qd_font_cjk_28;
    if (font == font_)
        return;
    auto previous = std::move(font_owner_);
    font_owner_ = std::move(owner);
    font_ = font;
    list_dirty_ = true;
    RenderList();
}

std::string Tab5PodcastPage::ShortDate(std::string_view time) {
    // "2026-10-03 07:26" → "10月03日 07:26"
    if (time.size() >= 16 && time[4] == '-' && time[7] == '-')
        return std::string(time.substr(5, 2)) + "月" + std::string(time.substr(8, 2)) + "日 " +
               std::string(time.substr(11, 5));
    return std::string(time);
}

const tab5_podcast::EpisodeList& Tab5PodcastPage::List() const {
    static const tab5_podcast::EpisodeList kEmpty;
    return episodes_ ? *episodes_ : kEmpty;
}

void Tab5PodcastPage::SetEpisodes(tab5_podcast::SharedEpisodes episodes) {
    // A new fetch always publishes a new list object; same pointer means no change.
    if (episodes == episodes_)
        return;
    const bool had_newest = !List().empty() && selected_id_ == List().front().id;
    episodes_ = std::move(episodes);
    // Follow a new episode when the newest one was selected (or nothing was).
    if (!List().empty() && (selected_id_ == 0 || had_newest))
        selected_id_ = List().front().id;
    list_dirty_ = true;
    if (IsVisible())
        RenderList();
}

void Tab5PodcastPage::RenderList() {
    list_dirty_ = false;
    const lv_font_t* font = font_ ? font_ : &qd_font_cjk_28;
    lv_obj_clean(list_);
    episode_tags_.clear();
    char summary[96];
    const auto& episodes = List();
    if (episodes.empty())
        std::snprintf(summary, sizeof(summary), "每日一期 · 来自 Muse · 还没有节目");
    else
        std::snprintf(summary, sizeof(summary), "每日一期 · 来自 Muse · 共 %u 期 · 最新 %s",
                      unsigned(episodes.size()), ShortDate(episodes.front().time).c_str());
    lv_label_set_text(summary_, summary);
    if (episodes.empty()) {
        auto* hint = Text(list_, "Muse 发布每日音乐电台后会出现在这里。", &qd_font_cjk_28, kMuted,
                          0, 0, 320);
        (void)hint;
        RenderEpisode();
        return;
    }
    if (std::none_of(episodes.begin(), episodes.end(),
                     [this](const auto& e) { return e.id == selected_id_; }))
        selected_id_ = episodes.front().id;
    for (size_t i = 0; i < episodes.size(); ++i) {
        const auto& e = episodes[i];
        const bool selected = e.id == selected_id_;
        auto* row = lv_obj_create(list_);
        lv_obj_set_size(row, 318, 96);
        lv_obj_set_style_radius(row, 18, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(selected ? 0x2a2550 : 0x10263a), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x2f3a63), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(row, lv_color_hex(selected ? kViolet : 0x2a4a62), 0);
        lv_obj_set_style_border_width(row, selected ? 2 : 1, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        auto* date = Text(row, ShortDate(e.time).c_str(), &qd_font_cjk_28,
                          selected ? kViolet : 0x7f9bb0, 18, 8, 290);
        auto* name = Text(row, tab5_podcast::DisplayText(e.title).c_str(), font, kText, 18, 46, 290);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_height(name, 40);
        for (auto* l : {date, name})
            lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
        episode_tags_.push_back(std::make_unique<Tag>(Tag{this, e.id}));
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* ev) {
                auto* tag = static_cast<Tag*>(lv_event_get_user_data(ev));
                if (tag->page->selected_id_ == tag->value)
                    return;
                tag->page->selected_id_ = tag->value;
                tag->page->RenderList();
            },
            LV_EVENT_CLICKED, episode_tags_.back().get());
    }
    RenderEpisode();
}

void Tab5PodcastPage::RenderEpisode() {
    const lv_font_t* font = font_ ? font_ : &qd_font_cjk_28;
    lv_obj_clean(tracks_);
    track_tags_.clear();
    const auto& episodes = List();
    auto it = std::find_if(episodes.begin(), episodes.end(),
                           [this](const auto& e) { return e.id == selected_id_; });
    if (it == episodes.end()) {
        lv_label_set_text(title_, "还没有节目");
        lv_label_set_text(meta_, "等待 Muse 发布今天的音乐电台");
        lv_label_set_text(script_, "");
        return;
    }
    const auto& e = *it;
    lv_obj_set_style_text_font(title_, font, 0);
    lv_label_set_text(title_, tab5_podcast::DisplayText(e.title).c_str());
    char meta[160];
    std::snprintf(meta, sizeof(meta), "%s · %u 首%s%s", ShortDate(e.time).c_str(),
                  unsigned(e.tracks.size()), e.from.empty() ? "" : " · ",
                  tab5_podcast::DisplayText(e.from).c_str());
    lv_label_set_text(meta_, meta);
    lv_obj_set_style_text_font(script_, font, 0);
    const std::string script = tab5_podcast::DisplayText(e.script);
    lv_label_set_text(script_, script.empty() ? "本期没有文案。" : script.c_str());
    lv_obj_scroll_to_y(script_box_, 0, LV_ANIM_OFF);

    if (e.tracks.empty())
        Text(tracks_, "本期没有歌单。", &qd_font_cjk_28, kMuted, 0, 0, 270);
    for (size_t i = 0; i < e.tracks.size(); ++i) {
        const auto& t = e.tracks[i];
        auto* row = lv_obj_create(tracks_);
        lv_obj_set_size(row, 272, 76);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x10263a), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x3a2f78), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        char number[12];
        std::snprintf(number, sizeof(number), "%02u", unsigned(i + 1));
        auto* num = Text(row, number, &qd_font_cjk_28, kViolet, 10, 18, 40);
        auto* name = Text(row, tab5_podcast::DisplayText(t.title).c_str(), font, kText, 54, 2, 210);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_height(name, 38);
        auto* artist = Text(row, tab5_podcast::DisplayText(t.artist).c_str(), &qd_font_cjk_28,
                            0x7f9bb0, 54, 38, 210);
        lv_label_set_long_mode(artist, LV_LABEL_LONG_DOT);
        lv_obj_set_height(artist, 34);
        for (auto* l : {num, name, artist})
            lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
        track_tags_.push_back(std::make_unique<Tag>(Tag{this, int(i)}));
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* ev) {
                auto* tag = static_cast<Tag*>(lv_event_get_user_data(ev));
                tag->page->PlayTrack(size_t(tag->value));
            },
            LV_EVENT_CLICKED, track_tags_.back().get());
    }
    lv_obj_scroll_to_y(tracks_, 0, LV_ANIM_OFF);
}

void Tab5PodcastPage::SetStatus(const char* text, uint32_t color) {
    lv_label_set_text(status_, text);
    lv_obj_set_style_text_color(status_, lv_color_hex(color), 0);
}

void Tab5PodcastPage::PlaySelected() {
    if (playing_) {
        if (callbacks_.stop)
            callbacks_.stop();
        SetStatus("已停止播放", kMuted);
        return;
    }
    const auto& episodes = List();
    auto it = std::find_if(episodes.begin(), episodes.end(),
                           [this](const auto& e) { return e.id == selected_id_; });
    if (it == episodes.end())
        return;
    if (!it->audio_url.empty() && callbacks_.play_url) {
        const bool ok = callbacks_.play_url(std::string(it->audio_url.data(), it->audio_url.size()),
                                            std::string(it->title.data(), it->title.size()));
        SetStatus(ok ? "正在连接节目音频…" : "节目音频暂时无法播放", ok ? kViolet : kPink);
        return;
    }
    if (!it->tracks.empty())
        PlayTrack(0);
}

void Tab5PodcastPage::PlayTrack(size_t index) {
    const auto& episodes = List();
    auto it = std::find_if(episodes.begin(), episodes.end(),
                           [this](const auto& e) { return e.id == selected_id_; });
    if (it == episodes.end() || index >= it->tracks.size() || !callbacks_.play_track)
        return;
    const auto& t = it->tracks[index];
    const bool ok = callbacks_.play_track(std::string(t.title.data(), t.title.size()),
                                          std::string(t.artist.data(), t.artist.size()));
    const std::string text = ok ? "正在请 Nabo 播放《" + tab5_podcast::DisplayText(t.title) + "》…"
                                : std::string("Nabo 正在忙，稍后再点一次");
    SetStatus(text.c_str(), ok ? kViolet : kPink);
}

void Tab5PodcastPage::SetPlayback(bool playing, const char* now_playing) {
    const std::string now = now_playing ? now_playing : "";
    if (playing == playing_ && now == now_playing_)
        return;
    playing_ = playing;
    now_playing_ = now;
    lv_label_set_text(play_label_, playing ? "停止播放" : "播放本期");
    lv_label_set_text(badge_label_, playing ? "正在播放" : "本期节目");
    lv_obj_set_style_bg_color(badge_, lv_color_hex(playing ? 0x6a3a6e : 0x3a2f78), 0);
    if (playing && !now.empty()) {
        const std::string text = "正在播放：" + tab5_podcast::DisplayText(now);
        SetStatus(text.c_str(), kPink);
    }
}

void Tab5PodcastPage::Tick() {
    if (!IsVisible())
        return;
    BindFont();
    if ((++phase_ & 1U) != 0)
        return;  // ~12 redraws/s of one bounded area
    const int level = playing_ && callbacks_.level ? std::clamp(callbacks_.level(), 0, 100) : 0;
    bool changed = false;
    for (int i = 0; i < kBars; ++i) {
        int target;
        if (playing_) {
            // Two travelling ripples scaled by the real output level.
            const int a = 40 - std::abs((i * 11 + int(phase_) * 6) % 80 - 40);
            const int b = 24 - std::abs((i * 7 + int(phase_) * 9 + 31) % 48 - 24);
            target = std::clamp(12 + level * (a + b + 10) / 70, 8, 84);
        } else {
            // Idle: a slow breathing wave so the panel still feels alive.
            target = 8 + (14 - std::abs((i * 3 + int(phase_)) % 28 - 14)) / 2;
        }
        const int next = (int(heights_[i]) * 2 + target + 1) / 3;
        changed |= next != heights_[i];
        heights_[i] = static_cast<uint8_t>(next);
    }
    if (changed)
        lv_obj_invalidate(wave_);
}

void Tab5PodcastPage::DrawWave(lv_event_t* event) {
    auto* self = static_cast<Tab5PodcastPage*>(lv_event_get_user_data(event));
    auto* object = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* layer = lv_event_get_layer(event);
    if (!self || !object || !layer)
        return;
    lv_area_t bounds;
    lv_obj_get_coords(object, &bounds);
    lv_draw_rect_dsc_t bar;
    lv_draw_rect_dsc_init(&bar);
    bar.radius = LV_RADIUS_CIRCLE;
    bar.border_width = 0;
    bar.bg_opa = self->playing_ ? LV_OPA_COVER : LV_OPA_50;
    const int mid = lv_obj_get_height(object) / 2;
    const int pitch = lv_obj_get_width(object) / kBars;
    for (int i = 0; i < kBars; ++i) {
        const int h = self->heights_[i];
        const int x = bounds.x1 + i * pitch + 2;
        const int y = bounds.y1 + mid - h / 2;
        bar.bg_color = self->colors_[i];
        lv_area_t area{x, y, x + pitch - 5, y + h - 1};
        lv_draw_rect(layer, &bar, &area);
    }
}
