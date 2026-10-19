#include "tab5_podcast_page.h"
#include "tab5_ui_motion.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "display/lvgl_display/lvgl_font.h"

LV_FONT_DECLARE(qd_font_lxgw_28);
LV_FONT_DECLARE(qd_font_lxgw_36);
LV_FONT_DECLARE(qd_font_cjk_28);
LV_FONT_DECLARE(qd_font_lyric_48);

namespace {
constexpr uint32_t kPage = 0x080b12, kPanel = 0x131a28, kChip = 0x1a2131;
constexpr uint32_t kAmber = 0xf7b955, kPressed = 0x273149, kText = 0xf5efe4;
constexpr uint32_t kMuted = 0x8b93a5, kBody = 0xc8cbd3, kError = 0xff6f91;
constexpr int kColX = 616, kColW = 640, kRowW = 616;
constexpr int kContentY = 350, kContentH = 226;

lv_obj_t* Box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t fill, uint32_t border,
              int radius) {
    auto* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
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
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_pad_row(o, gap, 0);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(o, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(o, lv_color_hex(kMuted), LV_PART_SCROLLBAR);
    lv_obj_set_style_width(o, 4, LV_PART_SCROLLBAR);
    return o;
}
}  // namespace

Tab5PodcastPage::Tab5PodcastPage(lv_obj_t* parent, Callbacks callbacks)
    : callbacks_(std::move(callbacks)) {
    body_font_ = qd_font_cjk_28;
    large_font_ = qd_font_lyric_48;
    root_ = Box(parent, 0, 0, 1280, 720, kPage, kPage, 0);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    Build();
}

void Tab5PodcastPage::Build() {
    Text(root_, "NABO 电台", &qd_font_lxgw_36, kAmber, 24, 6, 200);
    summary_ = Text(root_, "每日一期 · 来自 Muse", &qd_font_cjk_28, kMuted, 240, 12, 624);
    lv_label_set_long_mode(summary_, LV_LABEL_LONG_DOT);
    lv_obj_set_height(summary_, 40);

    auto button = [](lv_obj_t* parent, int x, int y, int w, int h, uint32_t fill, const char* text,
                     uint32_t color, lv_event_cb_t callback, void* data) {
        auto* pad = Box(parent, x, y, w, h, fill, fill, h / 2);
        lv_obj_add_flag(pad, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(pad, lv_color_hex(fill == kAmber ? 0xffd690 : kPressed),
                                  LV_STATE_PRESSED);
        tab5_ui::AddPressFeel(pad);
        auto* label = Text(pad, text, &qd_font_cjk_28, color, 0, 0, w);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(label);
        lv_obj_add_event_cb(pad, callback, LV_EVENT_CLICKED, data);
        return label;
    };
    button(
        root_, 1088, 8, 168, 50, kChip, "返回应用", kText,
        [](lv_event_t* event) {
            auto* page = static_cast<Tab5PodcastPage*>(lv_event_get_user_data(event));
            if (page->callbacks_.back)
                page->callbacks_.back();
        },
        this);
    refresh_label_ = button(
        root_, 888, 8, 184, 50, kChip, "刷新节目", kText,
        [](lv_event_t* event) {
            static_cast<Tab5PodcastPage*>(lv_event_get_user_data(event))->Refresh();
        },
        this);

    // One studio view and reader are shared with FM by Tab5NativeApps.
    episode_label_ = Text(root_, "每日音乐 · 本期节目", &qd_font_cjk_28, kMuted, kColX, 76, 440);
    lv_obj_set_height(episode_label_, 38);
    lv_label_set_long_mode(episode_label_, LV_LABEL_LONG_DOT);
    button(
        root_, 1080, 68, 176, 50, kPage, "往期节目 >", kMuted,
        [](lv_event_t* event) {
            static_cast<Tab5PodcastPage*>(lv_event_get_user_data(event))->SelectPane(3);
        },
        this);
    meta_ = Text(root_, "Nabo 的每日音乐来信", &qd_font_cjk_28, kMuted, kColX, 124, kColW);
    lv_label_set_long_mode(meta_, LV_LABEL_LONG_DOT);
    lv_obj_set_height(meta_, 36);
    // Long lyrics wrap at 48 px and can be scrolled vertically. No marquee
    // timer, scaled layer or second framebuffer competes with the studio.
    hero_ = ScrollColumn(root_, kColX, 164, kColW, 106, 0);
    title_ = Text(hero_, "今天，听 Nabo 说说", &large_font_, kText, 0, 0, kRowW);
    lv_obj_set_style_text_line_space(title_, 2, 0);
    Box(root_, kColX, 282, kColW, 1, 0x30394b, 0x30394b, 0);

    const char* tabs[] = {"节目文稿", "推荐歌单", "收藏歌单"};
    for (int i = 0; i < 3; ++i) {
        pane_labels_[i] = button(
            root_, kColX + i * 216, 290, 208, 48, kPage, tabs[i], kMuted,
            [](lv_event_t* event) {
                auto* page = static_cast<Tab5PodcastPage*>(lv_event_get_user_data(event));
                for (int pane = 0; pane < 3; ++pane)
                    if (lv_event_get_current_target(event) == page->pane_buttons_[pane]) {
                        page->SelectPane(pane);
                        break;
                    }
            },
            this);
        lv_label_set_long_mode(pane_labels_[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_height(pane_labels_[i], 36);
        pane_buttons_[i] = lv_obj_get_parent(pane_labels_[i]);
        lv_obj_set_style_radius(pane_buttons_[i], 0, 0);
        lv_obj_set_style_border_side(pane_buttons_[i], LV_BORDER_SIDE_BOTTOM, 0);
    }
    Box(root_, kColX, 338, kColW, 1, 0x30394b, 0x30394b, 0);
    script_heading_ = pane_labels_[0];
    script_box_ = ScrollColumn(root_, kColX, kContentY, kColW, kContentH, 8);
    tracks_ = ScrollColumn(root_, kColX, kContentY, kColW, kContentH, 8);
    list_ = ScrollColumn(root_, kColX, kContentY, kColW, kContentH, 10);
    favorites_list_ = ScrollColumn(root_, kColX, kContentY, kColW, kContentH, 8);
    Box(root_, kColX, 586, kColW, 1, 0x30394b, 0x30394b, 0);
    status_ =
        Text(root_, "选一期节目，和 Nabo 一起听。", &qd_font_cjk_28, kMuted, kColX, 594, kColW);
    lv_obj_set_height(status_, 38);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_DOT);

    play_label_ = button(
        root_, kColX, 638, 320, 58, kAmber, "播放播客", kPage,
        [](lv_event_t* event) {
            static_cast<Tab5PodcastPage*>(lv_event_get_user_data(event))->PlaySelected();
        },
        this);
    songs_label_ = button(
        root_, kColX + 336, 638, 304, 58, kChip, "播放推荐歌曲", kText,
        [](lv_event_t* event) {
            static_cast<Tab5PodcastPage*>(lv_event_get_user_data(event))->PlaySongs();
        },
        this);
    play_button_ = lv_obj_get_parent(play_label_);
    songs_button_ = lv_obj_get_parent(songs_label_);
    play_icon_ = Text(play_button_, LV_SYMBOL_PLAY, &lv_font_montserrat_20, kPage, 25, 18, 24);
    songs_icon_ = Text(songs_button_, LV_SYMBOL_PLAY, &lv_font_montserrat_20, kText, 25, 18, 24);
    for (auto* label : {play_label_, songs_label_}) {
        lv_obj_set_width(label, 244);
        lv_obj_align(label, LV_ALIGN_CENTER, 18, 0);
    }

    lv_obj_add_event_cb(
        script_box_,
        [](lv_event_t* event) {
            const auto code = lv_event_get_code(event);
            if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED)
                return;
            auto* page = static_cast<Tab5PodcastPage*>(lv_event_get_user_data(event));
            page->manual_scroll_ = true;
            page->manual_scroll_started_ = lv_tick_get();
        },
        LV_EVENT_ALL, this);
    RenderList();
    SelectPane(0);
}

void Tab5PodcastPage::SelectPane(int pane) {
    if (pane < 0 || pane >= 4)
        return;
    pane_ = pane;
    lv_obj_t* columns[] = {script_box_, tracks_, favorites_list_, list_};
    for (int i = 0; i < 4; ++i) {
        if (i == pane)
            lv_obj_remove_flag(columns[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(columns[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 3; ++i) {
        lv_obj_set_style_border_width(pane_buttons_[i], i == pane ? 3 : 0, 0);
        lv_obj_set_style_border_color(pane_buttons_[i], lv_color_hex(kAmber), 0);
        lv_obj_set_style_text_color(pane_labels_[i], lv_color_hex(i == pane ? kAmber : kMuted), 0);
    }
    if (pane == 0 && active_cue_ >= 0)
        transcript_scroll_pending_ = true;
    if (pane == 2)
        RenderFavorites();
    UpdatePlayLabels();
}

void Tab5PodcastPage::Show() {
    BindFont();
    RefreshFavorites();
    if (list_dirty_)
        RenderList();
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
}

void Tab5PodcastPage::Refresh() {
    if (refreshing_ || !callbacks_.refresh)
        return;
    refreshing_ = true;
    refresh_started_ = lv_tick_get();
    lv_label_set_text(refresh_label_, "刷新中…");
    callbacks_.refresh();
}

void Tab5PodcastPage::SetRefreshResult(bool ok, uint32_t poll_count) {
    if (poll_count == last_poll_count_)
        return;
    last_poll_count_ = poll_count;
    if (!refreshing_)
        return;
    refreshing_ = false;
    lv_label_set_text(refresh_label_, ok ? "刷新完成" : "刷新失败");
    if (!playing_)
        SetStatus(ok ? "已获取最新节目" : "刷新失败，保留上次内容，可点按重试",
                  ok ? kMuted : kError);
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
    body_font_.fallback = font;
    large_font_.fallback = &body_font_;
    lv_obj_set_style_text_font(status_, &body_font_, 0);
    lv_obj_set_style_text_font(meta_, &body_font_, 0);
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
    const lv_font_t* font = &body_font_;
    lv_obj_clean(list_);
    episode_tags_.clear();
    char summary[96];
    const auto& episodes = List();
    if (episodes.empty())
        std::snprintf(summary, sizeof(summary), "每日音乐 · 来自 Muse");
    else
        std::snprintf(summary, sizeof(summary), "每日音乐 · Muse · 共 %u 期",
                      unsigned(episodes.size()));
    lv_label_set_text(summary_, summary);
    if (episodes.empty()) {
        auto* hint = Text(list_, "Muse 发布每日音乐电台后会出现在这里。", &qd_font_cjk_28, kMuted,
                          0, 0, kRowW);
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
        lv_obj_set_size(row, kRowW, 100);
        lv_obj_set_style_radius(row, 18, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(selected ? 0x302719 : kPanel), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(kPressed), LV_STATE_PRESSED);
        tab5_ui::AddPressFeel(row);
        lv_obj_set_style_border_color(row, lv_color_hex(selected ? kAmber : kPanel), 0);
        lv_obj_set_style_border_width(row, selected ? 2 : 1, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        auto* date = Text(row, ShortDate(e.time).c_str(), &qd_font_cjk_28,
                          selected ? kAmber : kMuted, 18, 8, kRowW - 36);
        auto* name =
            Text(row, tab5_podcast::DisplayText(e.title).c_str(), font, kText, 18, 46, kRowW - 36);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_height(name, 40);
        for (auto* l : {date, name})
            lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
        episode_tags_.push_back(std::make_unique<Tag>(Tag{this, e.id}));
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* ev) {
                auto* tag = static_cast<Tag*>(lv_event_get_user_data(ev));
                if (tag->page->selected_id_ == tag->value) {
                    tag->page->SelectPane(0);
                    return;
                }
                auto* page = tag->page;
                page->selected_id_ = tag->value;
                page->RenderList();
                page->SelectPane(0);
            },
            LV_EVENT_CLICKED, episode_tags_.back().get());
    }
    RenderEpisode();
}

void Tab5PodcastPage::RenderEpisode() {
    UpdatePlayLabels();
    const lv_font_t* font = &body_font_;
    lv_obj_clean(tracks_);
    track_tags_.clear();
    track_rows_.clear();
    track_favorite_labels_.clear();
    lv_obj_clean(script_box_);
    cue_rows_.clear();
    active_cue_ = -1;
    manual_scroll_ = false;
    transcript_scroll_pending_ = false;
    script_ = Text(script_box_, "", font, kBody, 0, 0, kRowW);
    lv_obj_set_style_text_line_space(script_, 8, 0);
    lv_label_set_text(script_heading_, "节目文稿");
    const auto& episodes = List();
    auto it = std::find_if(episodes.begin(), episodes.end(),
                           [this](const auto& e) { return e.id == selected_id_; });
    if (it == episodes.end()) {
        episode_title_ = "今天，听 Nabo 说说";
        episode_detail_ = "等待 Muse 发布今天的音乐电台";
        lv_label_set_text(episode_label_, "每日音乐 · 本期节目");
        lv_label_set_text(pane_labels_[1], "推荐歌单");
        lv_label_set_text(script_,
                          "每日节目发布后，文稿和推荐歌单会出现在这里。\n可以点右上角刷新节目。");
        UpdateHero();
        return;
    }
    const auto& e = *it;
    episode_title_ = tab5_podcast::DisplayText(e.title);
    const std::string date = ShortDate(e.time) + " · 本期节目";
    lv_label_set_text(episode_label_, date.c_str());
    char meta[160];
    std::snprintf(meta, sizeof(meta), "%u 首推荐%s%s", unsigned(e.tracks.size()),
                  e.from.empty() ? "" : " · ", tab5_podcast::DisplayText(e.from).c_str());
    episode_detail_ = meta;
    lv_label_set_text(pane_labels_[1], "推荐歌单");
    UpdateHero();
    lv_obj_set_style_text_font(script_, font, 0);
    const std::string script = tab5_podcast::DisplayText(e.script);
    lv_label_set_text(script_, script.empty() ? "本期没有文案。" : script.c_str());
    if (!e.cues.empty()) {
        lv_obj_add_flag(script_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(script_heading_, "节目文稿");
        for (const auto& cue : e.cues) {
            const std::string spoken =
                (cue.music ? "音乐 · " : "") + tab5_podcast::DisplayText(cue.text);
            auto* row = Text(script_box_, spoken.c_str(), font, kBody, 0, 0, kRowW);
            lv_obj_set_style_text_line_space(row, 8, 0);
            lv_obj_set_style_pad_ver(row, 6, 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(0x302719), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
            cue_rows_.push_back(row);
        }
    }
    lv_obj_scroll_to_y(script_box_, 0, LV_ANIM_OFF);

    if (e.tracks.empty())
        Text(tracks_, "本期没有歌单。", &qd_font_cjk_28, kMuted, 0, 0, kRowW);
    RenderSongRows(tracks_, e.tracks, false);
    lv_obj_scroll_to_y(tracks_, 0, LV_ANIM_OFF);
    HighlightNowPlaying();
}

void Tab5PodcastPage::RenderSongRows(lv_obj_t* parent, const nabo_song_favorites::List& songs,
                                     bool saved) {
    const lv_font_t* font = &body_font_;
    auto& tags = saved ? favorite_tags_ : track_tags_;
    auto& rows = saved ? favorite_rows_ : track_rows_;
    for (size_t i = 0; i < songs.size(); ++i) {
        const auto& t = songs[i];
        auto* row = lv_obj_create(parent);
        lv_obj_set_size(row, kRowW, 84);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(kPanel), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(kPressed), LV_STATE_PRESSED);
        tab5_ui::AddPressFeel(row);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        char number[12];
        std::snprintf(number, sizeof(number), "%02u", unsigned(i + 1));
        auto* num = Text(row, number, &qd_font_cjk_28, kAmber, 10, 18, 40);
        auto* name =
            Text(row, tab5_podcast::DisplayText(t.title).c_str(), font, kText, 64, 4, kRowW - 198);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_height(name, 38);
        auto* artist = Text(row, tab5_podcast::DisplayText(t.artist).c_str(), font, kMuted, 64, 42,
                            kRowW - 198);
        lv_label_set_long_mode(artist, LV_LABEL_LONG_DOT);
        lv_obj_set_height(artist, 34);
        for (auto* l : {num, name, artist})
            lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
        tags.push_back(std::make_unique<Tag>(Tag{this, int(i), saved}));
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* ev) {
                auto* tag = static_cast<Tag*>(lv_event_get_user_data(ev));
                if (tag->saved)
                    tag->page->PlayFavorite(size_t(tag->value));
                else
                    tag->page->PlayTrack(size_t(tag->value));
            },
            LV_EVENT_CLICKED, tags.back().get());
        auto* favorite = Box(row, kRowW - 120, 12, 110, 60, kChip, kChip, 16);
        lv_obj_add_flag(favorite, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(favorite, lv_color_hex(kPressed), LV_STATE_PRESSED);
        tab5_ui::AddPressFeel(favorite);
        const bool on =
            saved || (favorites_.songs && nabo_song_favorites::Contains(*favorites_.songs, t));
        auto* mark = Text(favorite, on ? "已收藏" : "收藏", &qd_font_cjk_28, on ? kAmber : kMuted,
                          0, 0, 110);
        lv_label_set_long_mode(mark, LV_LABEL_LONG_CLIP);
        lv_obj_set_height(mark, 36);
        lv_obj_set_style_text_align(mark, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(mark);
        lv_obj_add_event_cb(
            favorite,
            [](lv_event_t* event) {
                auto* tag = static_cast<Tag*>(lv_event_get_user_data(event));
                tag->page->ToggleFavorite(size_t(tag->value), tag->saved);
            },
            LV_EVENT_CLICKED, tags.back().get());
        if (!saved)
            track_favorite_labels_.push_back(mark);
        rows.push_back(row);
    }
}

void Tab5PodcastPage::RenderFavorites() {
    lv_obj_clean(favorites_list_);
    favorite_tags_.clear();
    favorite_rows_.clear();
    if (!favorites_.ready) {
        Text(favorites_list_,
             favorites_.error == nabo_song_favorites::Error::Corrupt
                 ? "收藏文件读取失败，请检查 SD 卡。"
                 : "正在读取收藏歌单，请确认 SD 卡已插入。",
             &body_font_, kMuted, 0, 0, kRowW);
    } else if (!favorites_.songs || favorites_.songs->empty()) {
        Text(favorites_list_, "还没有收藏歌曲。\n在推荐歌单里点“收藏”，喜欢的歌就会留在这里。",
             &body_font_, kMuted, 0, 0, kRowW);
    } else
        RenderSongRows(favorites_list_, *favorites_.songs, true);
    HighlightNowPlaying();
}

void Tab5PodcastPage::UpdateFavoriteButtons() {
    const auto& episodes = List();
    auto episode = std::find_if(episodes.begin(), episodes.end(),
                                [this](const auto& item) { return item.id == selected_id_; });
    if (episode == episodes.end())
        return;
    for (size_t i = 0; i < track_favorite_labels_.size() && i < episode->tracks.size(); ++i) {
        const bool on = favorites_.songs &&
                        nabo_song_favorites::Contains(*favorites_.songs, episode->tracks[i]);
        lv_label_set_text(track_favorite_labels_[i], on ? "已收藏" : "收藏");
        lv_obj_set_style_text_color(track_favorite_labels_[i], lv_color_hex(on ? kAmber : kMuted),
                                    0);
    }
}

void Tab5PodcastPage::RefreshFavorites() {
    if (!callbacks_.favorites)
        return;
    auto snapshot = callbacks_.favorites();
    if (favorites_seen_ && snapshot.revision == favorites_.revision)
        return;
    const bool changed = !favorites_seen_ || snapshot.songs != favorites_.songs ||
                         snapshot.ready != favorites_.ready;
    const bool was_pending = favorites_.pending;
    favorites_ = std::move(snapshot);
    favorites_seen_ = true;
    if (changed) {
        UpdateFavoriteButtons();
        if (pane_ == 2)
            RenderFavorites();
    }
    if (favorites_.error == nabo_song_favorites::Error::Write)
        SetStatus("收藏保存失败，请检查 SD 卡", kError);
    else if (was_pending && !favorites_.pending)
        SetStatus("收藏歌单已保存", kMuted);
}

void Tab5PodcastPage::ToggleFavorite(size_t index, bool saved) {
    if (!callbacks_.toggle_favorite)
        return;
    nabo_song_favorites::Track song;
    if (saved) {
        if (!favorites_.songs || index >= favorites_.songs->size())
            return;
        song = (*favorites_.songs)[index];
    } else {
        const auto& episodes = List();
        auto episode = std::find_if(episodes.begin(), episodes.end(),
                                    [this](const auto& item) { return item.id == selected_id_; });
        if (episode == episodes.end() || index >= episode->tracks.size())
            return;
        song = episode->tracks[index];
    }
    const auto result = callbacks_.toggle_favorite(song);
    RefreshFavorites();
    using Change = nabo_song_favorites::Change;
    SetStatus(result == Change::Added     ? "已加入收藏，正在保存…"
              : result == Change::Removed ? "已取消收藏，正在保存…"
              : result == Change::Full    ? "收藏已满，请先移除一首"
                                          : "收藏暂不可用，请检查 SD 卡",
              result == Change::Added || result == Change::Removed ? kAmber : kError);
}

void Tab5PodcastPage::PlayFavorite(size_t index) {
    if (!favorites_.songs || index >= favorites_.songs->size() || !callbacks_.play_favorite)
        return;
    const auto song = (*favorites_.songs)[index];
    const bool ok = callbacks_.play_favorite(std::string(song.title), std::string(song.artist),
                                             std::string(song.song_id));
    if (ok) {
        playback_kind_ = PlaybackKind::Favorites;
        playback_episode_id_ = 0;
        now_title_ = tab5_podcast::DisplayText(song.title);
        now_artist_ = tab5_podcast::DisplayText(song.artist);
        now_line_.clear();
        UpdatePlayLabels();
    }
    SetStatus(ok ? "正在获取收藏歌曲…" : "收藏歌曲暂时无法播放", ok ? kAmber : kError);
}

void Tab5PodcastPage::SetStatus(const char* text, uint32_t color) {
    lv_label_set_text(status_, text);
    lv_obj_set_style_text_color(status_, lv_color_hex(color), 0);
}

void Tab5PodcastPage::PlaySelected() {
    if (playing_ && playback_kind_ == PlaybackKind::Podcast &&
        playback_episode_id_ == selected_id_) {
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
        if (ok) {
            playback_kind_ = PlaybackKind::Podcast;
            playback_episode_id_ = it->id;
            UpdatePlayLabels();
        }
        SetStatus(ok ? "正在连接播客音频…" : "播客音频暂时无法播放", ok ? kAmber : kError);
        return;
    }
    SetStatus("本期原声播客尚未上传，可单独播放推荐歌曲", kMuted);
}

void Tab5PodcastPage::PlaySongs() {
    if (playing_ && playback_kind_ == PlaybackKind::Favorites) {
        if (callbacks_.stop)
            callbacks_.stop();
        SetStatus("已停止播放", kMuted);
        return;
    }
    if (pane_ == 2) {
        PlayFavorite(0);
        return;
    }
    if (playing_ && playback_kind_ == PlaybackKind::Songs && playback_episode_id_ == selected_id_) {
        if (callbacks_.stop)
            callbacks_.stop();
        SetStatus("已停止播放", kMuted);
        return;
    }
    PlayTrack(0);  // Retain the NAS full-song lookup and ordered playlist.
}

void Tab5PodcastPage::UpdatePlayLabels() {
    const bool active = playing_ && playback_episode_id_ == selected_id_;
    lv_label_set_text(play_label_,
                      active && playback_kind_ == PlaybackKind::Podcast ? "停止播客" : "播放播客");
    const bool favorite = playback_kind_ == PlaybackKind::Favorites && playing_;
    lv_label_set_text(songs_label_, favorite     ? "停止收藏歌曲"
                                    : pane_ == 2 ? "播放收藏歌曲"
                                    : active && playback_kind_ == PlaybackKind::Songs
                                        ? "停止推荐歌曲"
                                        : "播放推荐歌曲");
    UpdateTransport();
    UpdateHero();
}

void Tab5PodcastPage::PlayTrack(size_t index) {
    const auto& episodes = List();
    auto it = std::find_if(episodes.begin(), episodes.end(),
                           [this](const auto& e) { return e.id == selected_id_; });
    if (it == episodes.end() || index >= it->tracks.size() || !callbacks_.play_track)
        return;
    const auto& t = it->tracks[index];
    const bool ok = callbacks_.play_track(it->id, int(index), int(it->tracks.size()));
    if (ok) {
        playback_kind_ = PlaybackKind::Songs;
        playback_episode_id_ = it->id;
        now_title_.clear();
        now_artist_.clear();
        now_line_.clear();
        UpdatePlayLabels();
    }
    const std::string text = ok ? "正在从网易云获取《" + tab5_podcast::DisplayText(t.title) +
                                      "》，之后按歌单顺序播放…"
                                : std::string("暂时无法播放，稍后再试");
    SetStatus(text.c_str(), ok ? kAmber : kError);
}

void Tab5PodcastPage::UpdateTransport() {
    const bool songs =
        playback_kind_ == PlaybackKind::Songs || playback_kind_ == PlaybackKind::Favorites;
    const bool active = playing_ && (playback_kind_ == PlaybackKind::Favorites ||
                                     playback_episode_id_ == selected_id_);
    const int mode = int(songs) | (active ? 2 : 0);
    if (mode == transport_mode_)
        return;
    transport_mode_ = mode;
    auto* primary = songs ? songs_button_ : play_button_;
    auto* secondary = songs ? play_button_ : songs_button_;
    lv_obj_set_pos(primary, kColX, 638);
    lv_obj_set_width(primary, 320);
    lv_obj_set_pos(secondary, kColX + 336, 638);
    lv_obj_set_width(secondary, 304);
    for (auto* button : {primary, secondary}) {
        lv_obj_set_style_bg_color(button, lv_color_hex(button == primary ? kAmber : kChip), 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(button == primary ? 0xffd690 : kPressed),
                                  LV_STATE_PRESSED);
    }
    lv_obj_set_style_text_color(play_label_, lv_color_hex(songs ? kText : kPage), 0);
    lv_obj_set_style_text_color(play_icon_, lv_color_hex(songs ? kText : kPage), 0);
    lv_obj_set_style_text_color(songs_label_, lv_color_hex(songs ? kPage : kText), 0);
    lv_obj_set_style_text_color(songs_icon_, lv_color_hex(songs ? kPage : kText), 0);
    lv_label_set_text(play_icon_, active && !songs ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
    lv_label_set_text(songs_icon_, active && songs ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
}

void Tab5PodcastPage::UpdateHero() {
    const bool song = playback_kind_ != PlaybackKind::Podcast && !now_title_.empty();
    if (hero_mode_ != int(song)) {
        hero_mode_ = int(song);
        lv_obj_set_style_text_color(title_, lv_color_hex(song ? kAmber : kText), 0);
    }
    std::string heading = song ? (now_line_.empty() ? "等待歌词" : now_line_) : episode_title_;
    if (heading.empty())
        heading = "今天，听 Nabo 说说";
    std::string meta = song ? now_title_ : episode_detail_;
    if (song && !now_artist_.empty())
        meta += " · " + now_artist_;
    if (heading != lv_label_get_text(title_)) {
        lv_label_set_text(title_, heading.c_str());
        lv_obj_scroll_to_y(hero_, 0, LV_ANIM_OFF);
    }
    if (song && song_trial_)
        meta = "仅试听 · " + meta;
    if (meta != lv_label_get_text(meta_))
        lv_label_set_text(meta_, meta.c_str());
}

void Tab5PodcastPage::SetNowPlaying(const char* title, const char* artist, const char* line) {
    const std::string next_title = tab5_podcast::DisplayText(title ? title : "");
    const bool changed = next_title != now_title_;
    now_title_ = next_title;
    now_artist_ = tab5_podcast::DisplayText(artist ? artist : "");
    now_line_ = tab5_podcast::DisplayText(line ? line : "");
    UpdateHero();
    if (changed)
        HighlightNowPlaying();
}

// Marks the selected episode's row whose title matches the song now playing.
void Tab5PodcastPage::HighlightNowPlaying() {
    const auto& episodes = List();
    auto it = std::find_if(episodes.begin(), episodes.end(),
                           [this](const auto& e) { return e.id == selected_id_; });
    for (size_t i = 0; i < track_rows_.size(); ++i) {
        const bool on = playing_ && it != episodes.end() && i < it->tracks.size() &&
                        !now_title_.empty() &&
                        std::string_view(it->tracks[i].title) == now_title_;
        lv_obj_set_style_bg_color(track_rows_[i], lv_color_hex(on ? 0x302719 : kPanel), 0);
        lv_obj_set_style_border_color(track_rows_[i], lv_color_hex(kAmber), 0);
        lv_obj_set_style_border_width(track_rows_[i], on ? 2 : 0, 0);
        if (on)
            lv_obj_scroll_to_view(track_rows_[i], LV_ANIM_ON);
    }
    for (size_t i = 0;
         favorites_.songs && i < favorite_rows_.size() && i < favorites_.songs->size(); ++i) {
        const auto& song = (*favorites_.songs)[i];
        const bool on =
            playing_ && tab5_podcast::DisplayText(song.title) == now_title_ &&
            (now_artist_.empty() || tab5_podcast::DisplayText(song.artist) == now_artist_);
        lv_obj_set_style_bg_color(favorite_rows_[i], lv_color_hex(on ? 0x302719 : kPanel), 0);
        lv_obj_set_style_border_color(favorite_rows_[i], lv_color_hex(kAmber), 0);
        lv_obj_set_style_border_width(favorite_rows_[i], on ? 2 : 0, 0);
    }
}

void Tab5PodcastPage::SetPlayback(bool playing, const char* now_playing) {
    const std::string now = now_playing ? now_playing : "";
    if (playing == playing_ && now == now_playing_)
        return;
    playing_ = playing;
    now_playing_ = now;
    UpdatePlayLabels();
    HighlightNowPlaying();
    if (playing && !now.empty()) {
        const char* prefix = playback_kind_ == PlaybackKind::Podcast     ? "播客播放中 · "
                             : playback_kind_ == PlaybackKind::Favorites ? "收藏歌曲播放中 · "
                             : playback_kind_ == PlaybackKind::Songs     ? "推荐歌曲播放中 · "
                                                                         : "正在播放 · ";
        const std::string text = prefix + tab5_podcast::DisplayText(now);
        SetStatus(text.c_str(), kAmber);
    }
}

void Tab5PodcastPage::Tick() {
    if (!IsVisible())
        return;
    if (refreshing_ && uint32_t(lv_tick_get() - refresh_started_) >= 45000) {
        refreshing_ = false;
        lv_label_set_text(refresh_label_, "刷新失败");
        if (!playing_)
            SetStatus("刷新超时，可点按重试", kError);
    }
    BindFont();
    UpdateTranscript();
    RefreshFavorites();
}

void Tab5PodcastPage::UpdateTranscript() {
    if (cue_rows_.empty())
        return;
    int next = -1;
    if (playing_ && playback_kind_ == PlaybackKind::Podcast &&
        playback_episode_id_ == selected_id_ && callbacks_.playback_position) {
        const auto& episodes = List();
        auto it = std::find_if(episodes.begin(), episodes.end(),
                               [this](const auto& e) { return e.id == selected_id_; });
        if (it != episodes.end()) {
            const auto position = callbacks_.playback_position(it->audio_url);
            if (position.matches)
                next = tab5_podcast::ActiveCue(it->cues, position.milliseconds);
        }
    }
    if (next != active_cue_) {
        if (active_cue_ >= 0 && size_t(active_cue_) < cue_rows_.size()) {
            lv_obj_set_style_text_color(cue_rows_[active_cue_], lv_color_hex(kBody), 0);
            lv_obj_set_style_bg_opa(cue_rows_[active_cue_], LV_OPA_TRANSP, 0);
        }
        active_cue_ = next;
        transcript_scroll_pending_ = next >= 0;
        if (next >= 0 && size_t(next) < cue_rows_.size()) {
            lv_obj_set_style_text_color(cue_rows_[next], lv_color_hex(kAmber), 0);
            lv_obj_set_style_bg_opa(cue_rows_[next], LV_OPA_COVER, 0);
        }
    }
    if (manual_scroll_ && uint32_t(lv_tick_get() - manual_scroll_started_) >= 8000)
        manual_scroll_ = false;
    if (pane_ == 0 && !manual_scroll_ && transcript_scroll_pending_ && active_cue_ >= 0 &&
        size_t(active_cue_) < cue_rows_.size()) {
        // A Muse segment can be a whole paragraph taller than the viewport.
        // Show its beginning, rather than scrolling to its last wrapped line.
        lv_obj_update_layout(script_box_);
        lv_obj_scroll_to_y(script_box_, lv_obj_get_y(cue_rows_[active_cue_]), LV_ANIM_ON);
        transcript_scroll_pending_ = false;
    }
}

nabo_radio::Mood Tab5PodcastPage::StudioMood(const char* state) const {
    nabo_radio::Mood mood;
    mood.link = nabo_radio::LinkFromState(state);
    mood.music = playback_kind_ == PlaybackKind::Songs || playback_kind_ == PlaybackKind::Favorites;
    if (playback_kind_ == PlaybackKind::Podcast && playback_episode_id_ == selected_id_ &&
        active_cue_ >= 0) {
        const auto& episodes = List();
        auto episode = std::find_if(episodes.begin(), episodes.end(),
                                    [this](const auto& item) { return item.id == selected_id_; });
        if (episode != episodes.end() && size_t(active_cue_) < episode->cues.size())
            mood.music = episode->cues[active_cue_].music;
    }
    return mood;
}
