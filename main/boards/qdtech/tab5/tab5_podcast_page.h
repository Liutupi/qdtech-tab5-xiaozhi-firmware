#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lvgl.h"
#include "tab5_playback_clock.h"
#include "tab5_podcast_model.h"

class LvglFont;

// "Muse 音乐电台": the daily music-radio episodes Muse publishes through the NAS relay.
// Episode list, full script, track list and an animated waveform that follows the real
// playback level. All methods run on the LVGL task with the display lock held.
class Tab5PodcastPage {
public:
    struct Callbacks {
        std::function<void()> back;
        std::function<void()> refresh;
        // Stream an episode's narrated audio. Returns false when playback could not start.
        std::function<bool(const std::string& url, const std::string& title)> play_url;
        // Play the episode's songs from index on (full songs resolved on the NAS).
        std::function<bool(int episode, int index, int count)> play_track;
        std::function<void()> stop;
        std::function<int()> level;  // 0..100 output level while music plays
        std::function<tab5_playback::Position(std::string_view)> playback_position;
        std::function<std::shared_ptr<LvglFont>()> text_font;  // full CJK font, may be null
    };

    Tab5PodcastPage(lv_obj_t* parent, Callbacks callbacks);
    lv_obj_t* object() const { return root_; }
    void Show();
    void Hide();
    bool IsVisible() const;
    void SetEpisodes(tab5_podcast::SharedEpisodes episodes);
    void SetRefreshResult(bool ok, uint32_t poll_count);
    void SetPlayback(bool playing, const char* now_playing);
    // Song title/artist and current lyric (or status) line while a track plays.
    void SetNowPlaying(const char* title, const char* artist, const char* line);
    void Tick();

private:
    static constexpr int kBars = 56;

    void Build();
    void Refresh();
    void RenderList();
    void RenderEpisode();
    void UpdateTranscript();
    void BindFont();
    void SetStatus(const char* text, uint32_t color);
    void PlaySelected();
    void PlaySongs();
    void UpdatePlayLabels();
    void PlayTrack(size_t index);
    static void DrawWave(lv_event_t* event);
    static std::string ShortDate(std::string_view time);

    Callbacks callbacks_;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* summary_ = nullptr;
    lv_obj_t* refresh_label_ = nullptr;
    bool refreshing_ = false;
    uint32_t last_poll_count_ = 0;
    uint32_t refresh_started_ = 0;
    lv_obj_t* list_ = nullptr;
    lv_obj_t* badge_ = nullptr;
    lv_obj_t* badge_label_ = nullptr;
    lv_obj_t* title_ = nullptr;
    lv_obj_t* meta_ = nullptr;
    lv_obj_t* play_label_ = nullptr;
    lv_obj_t* songs_label_ = nullptr;
    lv_obj_t* wave_ = nullptr;
    lv_obj_t* script_box_ = nullptr;
    lv_obj_t* script_ = nullptr;
    lv_obj_t* script_heading_ = nullptr;
    std::vector<lv_obj_t*> cue_rows_;
    int active_cue_ = -1;
    uint32_t manual_scroll_started_ = 0;
    bool manual_scroll_ = false;
    bool transcript_scroll_pending_ = false;
    lv_obj_t* tracks_ = nullptr;
    lv_obj_t* status_ = nullptr;

    tab5_podcast::SharedEpisodes episodes_;  // shared, never copied
    const tab5_podcast::EpisodeList& List() const;
    int selected_id_ = 0;
    bool list_dirty_ = true;
    bool playing_ = false;
    enum class PlaybackKind { None, Podcast, Songs };
    PlaybackKind playback_kind_ = PlaybackKind::None;
    int playback_episode_id_ = 0;
    std::string now_playing_;
    uint32_t phase_ = 0;
    std::array<uint8_t, kBars> heights_{};
    std::array<lv_color_t, kBars> colors_{};
    std::shared_ptr<LvglFont> font_owner_;
    const lv_font_t* font_ = nullptr;
    // Click payloads for list rows / track rows (rebuilt with their lists).
    struct Tag {
        Tab5PodcastPage* page;
        int value;
    };
    std::vector<std::unique_ptr<Tag>> episode_tags_;
    std::vector<std::unique_ptr<Tag>> track_tags_;
    std::vector<lv_obj_t*> track_rows_;
    std::string now_title_;
    void HighlightNowPlaying();
};
