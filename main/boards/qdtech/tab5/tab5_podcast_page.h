#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lvgl.h"
#include "nabo_radio_mood.h"
#include "nabo_song_favorites.h"
#include "tab5_playback_clock.h"
#include "tab5_podcast_model.h"

class LvglFont;

// "Muse 音乐电台": the daily music-radio episodes Muse publishes through the NAS relay.
// Episode list, synchronized script and track list beside the shared FM studio.
// All methods run on the LVGL task with the display lock held.
class Tab5PodcastPage {
public:
    struct Callbacks {
        std::function<void()> back;
        std::function<void()> refresh;
        // Stream an episode's narrated audio. Returns false when playback could not start.
        std::function<bool(const std::string& url, const std::string& title)> play_url;
        // Play the episode's songs from index on (full songs resolved on the NAS).
        std::function<bool(int episode, int index, int count)> play_track;
        std::function<bool(const std::string&, const std::string&, const std::string&)>
            play_favorite;
        std::function<nabo_song_favorites::Snapshot()> favorites;
        std::function<nabo_song_favorites::Change(const nabo_song_favorites::Track&)>
            toggle_favorite;
        std::function<void()> stop;
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
    void SetSongTrial(bool trial) {
        song_trial_ = trial;
        UpdateHero();
    }
    void Tick();
    nabo_radio::Mood StudioMood(const char* state) const;

private:
    void Build();
    void SelectPane(int pane);
    void Refresh();
    void RenderList();
    void RenderEpisode();
    void UpdateTranscript();
    void BindFont();
    void SetStatus(const char* text, uint32_t color);
    void PlaySelected();
    void PlaySongs();
    void UpdatePlayLabels();
    void UpdateTransport();
    void UpdateHero();
    void PlayTrack(size_t index);
    void PlayFavorite(size_t index);
    void ToggleFavorite(size_t index, bool saved);
    void RefreshFavorites();
    void RenderFavorites();
    void RenderSongRows(lv_obj_t* parent, const nabo_song_favorites::List& songs, bool saved);
    void UpdateFavoriteButtons();
    static std::string ShortDate(std::string_view time);

    Callbacks callbacks_;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* summary_ = nullptr;
    lv_obj_t* refresh_label_ = nullptr;
    bool refreshing_ = false;
    uint32_t last_poll_count_ = 0;
    uint32_t refresh_started_ = 0;
    lv_obj_t* list_ = nullptr;
    lv_obj_t* episode_label_ = nullptr;
    lv_obj_t* hero_ = nullptr;
    lv_obj_t* title_ = nullptr;
    lv_obj_t* meta_ = nullptr;
    lv_obj_t* play_label_ = nullptr;
    lv_obj_t* songs_label_ = nullptr;
    lv_obj_t* play_button_ = nullptr;
    lv_obj_t* songs_button_ = nullptr;
    lv_obj_t* play_icon_ = nullptr;
    lv_obj_t* songs_icon_ = nullptr;
    int transport_mode_ = -1;
    int hero_mode_ = -1;
    std::array<lv_obj_t*, 3> pane_buttons_{};
    std::array<lv_obj_t*, 3> pane_labels_{};
    int pane_ = 0;
    lv_obj_t* script_box_ = nullptr;
    lv_obj_t* script_ = nullptr;
    lv_obj_t* script_heading_ = nullptr;
    std::vector<lv_obj_t*> cue_rows_;
    int active_cue_ = -1;
    uint32_t manual_scroll_started_ = 0;
    bool manual_scroll_ = false;
    bool transcript_scroll_pending_ = false;
    lv_obj_t* tracks_ = nullptr;
    lv_obj_t* favorites_list_ = nullptr;
    lv_obj_t* status_ = nullptr;
    nabo_song_favorites::Snapshot favorites_;
    bool favorites_seen_ = false;

    tab5_podcast::SharedEpisodes episodes_;  // shared, never copied
    const tab5_podcast::EpisodeList& List() const;
    int selected_id_ = 0;
    bool list_dirty_ = true;
    bool playing_ = false;
    bool song_trial_ = false;
    enum class PlaybackKind { None, Podcast, Songs, Favorites };
    PlaybackKind playback_kind_ = PlaybackKind::None;
    int playback_episode_id_ = 0;
    std::string now_playing_;
    std::shared_ptr<LvglFont> font_owner_;
    const lv_font_t* font_ = nullptr;
    lv_font_t body_font_{};
    lv_font_t large_font_{};
    // Click payloads for list rows / track rows (rebuilt with their lists).
    struct Tag {
        Tab5PodcastPage* page;
        int value;
        bool saved = false;
    };
    std::vector<std::unique_ptr<Tag>> episode_tags_;
    std::vector<std::unique_ptr<Tag>> track_tags_;
    std::vector<lv_obj_t*> track_rows_;
    std::vector<lv_obj_t*> track_favorite_labels_;
    std::vector<std::unique_ptr<Tag>> favorite_tags_;
    std::vector<lv_obj_t*> favorite_rows_;
    std::string now_title_;
    std::string now_artist_;
    std::string now_line_;
    std::string episode_title_;
    std::string episode_detail_;
    void HighlightNowPlaying();
};
