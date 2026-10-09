#pragma once

#include <atomic>
#include <functional>
#include <array>
#include <memory>
#include <string>

#include "lvgl.h"
#include "tab5_home_page.h"
#include "tab5_icu_page.h"
#include "tab5_ir_remote_page.h"
#include "tab5_muse_inbox.h"
#include "tab5_podcast_page.h"

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
        std::function<int()> radio_level;
        std::function<tab5_playback::Position(std::string_view)> podcast_position;
        // NES / USB gamepad
        std::function<void()> start_nes;
        std::function<void()> stop_nes;
        std::function<void()> nes_play_pause;
        std::function<void()> nes_next;
        std::function<void()> nes_previous;
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
    static constexpr int kRows = 6;
    static constexpr int kWaveBars = 40;
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
    lv_obj_t* station_rows_[kRows] = {};
    lv_obj_t* station_names_[kRows] = {};
    lv_obj_t* station_page_label_ = nullptr;
    lv_obj_t* radio_wave_ = nullptr;
    std::array<uint8_t, kWaveBars> wave_heights_{};
    std::array<lv_color_t, kWaveBars> wave_colors_{};
    std::string current_station_name_;
    unsigned wave_phase_ = 0;
    int station_page_ = 0;
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
    void UpdateWave();
    static void DrawWave(lv_event_t* event);
    static void DrawNes(lv_event_t* event);
};
