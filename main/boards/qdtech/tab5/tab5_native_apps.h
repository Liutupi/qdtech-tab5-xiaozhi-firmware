#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "lvgl.h"
#include "tab5_home_page.h"
#include "tab5_icu_page.h"
#include "tab5_ir_remote_page.h"
#include "tab5_md_launcher.h"
#include "tab5_muse_inbox.h"
#include "tab5_podcast_page.h"
#include "tab5_radio_stage.h"

class LvglFont;

// A full-resolution second page for device controls and internet radio.
// All methods that touch LVGL are called while the display lock is held.
class Tab5NativeApps {
public:
    struct Actions {
        std::function<void()> reconfigure_wifi;
        std::function<std::string()> wifi_summary;
        std::function<int()> get_brightness;
        std::function<int()> get_volume;
        std::function<void(int)> set_brightness;
        std::function<void(int)> set_volume;
        std::function<void()> start_radio;
        std::function<void()> radio_play_pause;
        std::function<bool()> radio_play_requested;
        std::function<void()> radio_stop;
        std::function<void()> radio_next;
        std::function<void()> radio_previous;
        // 点歌 from the radio page: stop playback and open a voice turn.
        std::function<void()> ask_song;
        std::function<void(int)> radio_select;
        std::function<int()> station_count;
        std::function<std::string(int)> station_name;
        // Band name of a station (全国, 北京, 音乐, ...); empty when unknown.
        std::function<std::string(int)> station_band;
        std::function<int()> current_station;
        std::function<bool(int)> station_favorite;
        std::function<void(int)> toggle_favorite;
        std::function<int()> radio_level;
        std::function<tab5_playback::Position(std::string_view)> podcast_position;
        // NES / USB gamepad
        std::function<void()> start_nes;
        std::function<void()> stop_nes;
        std::function<void()> nes_play_pause;
        std::function<void()> nes_next;
        std::function<void()> nes_previous;
        std::function<void(int)> nes_select;
        std::function<void()> nes_status;
        std::function<int()> nes_rom_count;
        std::function<std::string(int)> nes_rom_name;
        std::function<int()> nes_rom_index;
        // Settings -> 固件升级 (check / install)
        std::function<void()> firmware_action;
        // Muse inbox: poll the NAS now / the user opened the inbox (mark read).
        std::function<void()> muse_refresh;
        std::function<void()> muse_opened;
        // Muse 音乐电台: stream an episode's narrated audio / ask Nabo to play a song.
        std::function<bool(const std::string& url, const std::string& title)> podcast_play_url;
        // Play an episode's tracks from index (NAS resolves each song on NetEase).
        std::function<bool(int episode, int index, int count)> podcast_play;
    };

    explicit Tab5NativeApps(lv_obj_t* screen);
    void SetActions(Actions actions);
    // Lets the radio studio write NABO's opaque frames straight to the panel.
    void SetDirectBlit(Tab5RadioStage::DirectBlit blit, void* context) {
        if (radio_stage_)
            radio_stage_->SetDirectBlit(blit, context);
    }
    void OpenApps();
    void OpenSettings();
    void OpenRadio(bool start_playback = true);
    void OpenNes();
    void OpenIr();
    // 米家中控 (Home Assistant scenes and devices; the IR remote opens from it).
    void OpenHomeHub();
    // Called with the display lock held.
    void SetHomeStatus(const tab5_home::Status& status);
    void OpenMuse();
    void OpenPodcast();
    // Called with the display lock held.
    void SetMuseInbox(const tab5_muse::Snapshot& snapshot);
    void OpenIcu(int mode = -1, const std::string& external_result = "", bool result_ok = true);
    static void Schedule(std::function<void()> action);
    // Feed one NES 256x240 RGB565 frame; scaled to 960x720 (4:3) on the game page.
    void SetNesFrame(const uint16_t* pixels, uint16_t width, uint16_t height);
    void SetNesStatus(const char* status);
    void SetNesPlaying(bool playing);
    void Close();
    void RefreshWifi();
    void RefreshSettings();
    // Called with the display lock held.
    void SetFirmwareStatus(const char* text, const char* button, int progress, bool busy);
    void RefreshStations();
    void SetRadioState(const char* station, const char* state, const char* meta);
    // Voice-assistant status on the radio/music page (点歌). Empty text hides it.
    // Must be called with the display lock held.
    void SetVoiceStatus(const char* text, bool active);
    void SetMusicLyric(const char* title, const char* artist, const char* line);
    void SetMusicLyricLine(const char* line);
    void SetMusicLyricsWindow(const char* title, const char* artist, const char* previous,
                              const char* current, const char* next);
    void ClearMusicLyrics();
    void Tick();
    bool IsVisible() const;
    bool IsRadioVisible() const;
    bool IsSettingsVisible() const;
    bool IsPodcastVisible() const;
    void SetPodcastNowPlaying(const char* title, const char* artist, const char* line);

private:
    lv_obj_t* root_ = nullptr;
    lv_obj_t* home_page_ = nullptr;
    lv_obj_t* settings_page_ = nullptr;
    lv_obj_t* radio_page_ = nullptr;
    lv_obj_t* game_page_ = nullptr;
    lv_obj_t* muse_page_ = nullptr;
    lv_obj_t* muse_list_ = nullptr;
    lv_obj_t* muse_status_ = nullptr;
    lv_obj_t* muse_url_ = nullptr;
    lv_obj_t* muse_entry_label_ = nullptr;
    tab5_muse::Snapshot muse_snapshot_;
    bool muse_list_dirty_ = false;
    const lv_font_t* muse_rendered_font_ = nullptr;
    std::shared_ptr<LvglFont> muse_font_owner_;
    void BuildMuse();
    void RenderMuseList(const tab5_muse::Snapshot& snapshot);
    std::unique_ptr<Tab5IcuPage> icu_page_;
    std::unique_ptr<Tab5IrRemotePage> ir_page_;
    std::unique_ptr<Tab5HomePage> mijia_page_;
    // Created on first open; episodes and playback state are kept here meanwhile.
    std::unique_ptr<Tab5PodcastPage> podcast_page_;
    tab5_podcast::SharedEpisodes podcast_episodes_;
    std::string radio_station_name_;
    lv_obj_t* podcast_entry_detail_ = nullptr;
    lv_obj_t* podcast_entry_label_ = nullptr;
    void UpdatePodcastEntry();
    lv_obj_t* game_canvas_ = nullptr;
    lv_obj_t* game_status_ = nullptr;
    lv_obj_t* game_select_panel_ = nullptr;
    lv_obj_t* game_play_panel_ = nullptr;
    lv_obj_t* game_rom_label_ = nullptr;
    lv_obj_t* game_exit_label_ = nullptr;
    // FC (NES, in this firmware) or MD (Mega Drive, launched into the ota_0 app).
    lv_obj_t* game_mode_btns_[2] = {};
    bool game_md_mode_ = false;
    int md_selected_ = 0;
    int game_list_start_ = 0;
    std::shared_ptr<const tab5_md::Catalog> md_catalog_;
    lv_obj_t* game_rom_rows_[8] = {};
    lv_obj_t* game_rom_names_[8] = {};
    lv_img_dsc_t game_img_{};
    // Allocated only if direct panel video cannot present an emulator frame.
    // The emulation task writes outside the LVGL lock while LVGL reads another slot.
    uint16_t* game_scaled_[3] = {};
    int game_write_idx_ = 0;
    // Published by the UI thread; the emulator checks it before allocating/scaling.
    std::atomic<bool> game_page_visible_{false};
    std::atomic<bool> game_playing_ui_{false};
    std::atomic<bool> game_fallback_error_{false};
    std::atomic<bool> game_stop_ui_pending_{false};
    bool game_pad_resync_ = false;
    int game_list_index_ = -1;
    int game_list_count_ = -1;
    lv_obj_t* wifi_label_ = nullptr;
    lv_obj_t* brightness_slider_ = nullptr;
    lv_obj_t* volume_slider_ = nullptr;
    lv_obj_t* brightness_value_ = nullptr;
    lv_obj_t* volume_value_ = nullptr;
    lv_obj_t* fw_status_ = nullptr;
    lv_obj_t* fw_bar_ = nullptr;
    lv_obj_t* fw_button_ = nullptr;
    lv_obj_t* fw_button_label_ = nullptr;
    bool fw_busy_ = false;
    lv_obj_t* radio_station_ = nullptr;
    lv_obj_t* radio_state_ = nullptr;
    lv_obj_t* radio_meta_ = nullptr;
    lv_obj_t* radio_lyric_ = nullptr;
    lv_obj_t* radio_lyric_previous_ = nullptr;
    lv_obj_t* radio_lyric_next_ = nullptr;
    lv_obj_t* radio_next_label_ = nullptr;
    lv_obj_t* radio_voice_ = nullptr;
    lv_obj_t* ask_song_label_ = nullptr;
    const lv_font_t* music_font_ = nullptr;
    std::shared_ptr<LvglFont> music_font_owner_;
    lv_obj_t* radio_play_label_ = nullptr;
    // NABO FM page: studio on the left, tuner on the right.
    struct RadioBand {
        std::string name;
        std::vector<int> stations;
    };
    std::unique_ptr<Tab5RadioStage> radio_stage_;
    lv_obj_t* radio_eyebrow_ = nullptr;
    lv_obj_t* radio_fav_ = nullptr;
    lv_obj_t* radio_strip_tag_ = nullptr;
    lv_obj_t* radio_bands_ = nullptr;
    lv_obj_t* radio_dial_ = nullptr;
    lv_obj_t* radio_prev_name_ = nullptr;
    lv_obj_t* radio_next_name_ = nullptr;
    lv_obj_t* radio_count_ = nullptr;
    lv_font_t* station_font_ = nullptr;  // 56 px WenKai from the SD card, if present
    std::vector<RadioBand> radio_band_list_;
    std::vector<uint8_t> radio_favorites_;
    int radio_band_ = -1;
    float dial_pos_ = 0;  // station position within the band, fractional while moving
    float dial_drag_from_ = 0;
    int dial_drag_dx_ = 0;
    bool dial_dragging_ = false;
    bool dial_gliding_ = false;
    int tune_index_ = -1;  // station to connect once the dial has rested
    uint32_t tune_due_ms_ = 0;
    // The service publishes a new station only once it starts; until then it still
    // reports the old one, which must not pull the dial back.
    int tune_sent_index_ = -1;
    uint32_t tune_sent_ms_ = 0;
    int radio_current_ = -1;
    bool radio_song_ = false;
    // Last styles applied, so repeated status updates do not repaint labels.
    const lv_font_t* lyric_font_ = nullptr;
    bool lyric_song_style_ = false;
    uint32_t radio_state_color_ = 0;
    bool radio_stage_active_ = false;
    std::string radio_link_state_;
    std::string current_station_name_;
    bool radio_playing_ = false;
    Actions actions_;

    static lv_obj_t* Card(lv_obj_t* parent, int x, int y, int width, int height,
                          uint32_t fill, uint32_t border, int radius);
    static lv_obj_t* Label(lv_obj_t* parent, const char* text, const lv_font_t* font,
                           uint32_t color, int x, int y, int width);
    static lv_obj_t* Button(lv_obj_t* parent, const char* text, int x, int y,
                            int width, int height, void (*callback)(lv_event_t*), void* user_data);
    static void SetLabelTextIfChanged(lv_obj_t* label, const char* text);
    void Show(lv_obj_t* page);
    // Fades through dark (backlight) when a different page became visible.
    void AnimateEnter(lv_obj_t* page);
    lv_obj_t* current_page_ = nullptr;
    void BuildHome();
    void BuildSettings();
    void BuildRadio();
    void SyncRadioTextFont();
    void BuildGame();
    bool EnsureGameFallbackBuffers();
    void FailGameFallback();
    void RefreshGameRoms();
    void ShowGameSelect();
    void ShowGamePlay();
    void ExitGame(bool playing);
    void SetGameMode(bool md);
    int GameListCount() const;
    int GameListIndex() const;
    std::string GameListName(int index) const;
    void SelectGame(int index, bool start);
    void LaunchMdGame();
    void TickRadio();
    void LoadStationFont();
    void RebuildRadioBands(bool reload_favorites = true);
    void SelectRadioBand(int band, bool tune);
    int BandPosition(int station) const;
    void SetDialPosition(float position);
    void GlideDial(int target);
    void StepStation(int delta);
    void ScheduleTune();
    void FollowCurrentStation();
    void RefreshTuner();
    nabo_radio::Mood RadioMood() const;
    static void DrawDial(lv_event_t* event);
    static void DrawHeart(lv_event_t* event);
    static void OnDial(lv_event_t* event);
    static void DrawNes(lv_event_t* event);
};
