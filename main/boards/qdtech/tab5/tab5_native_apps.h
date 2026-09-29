#pragma once

#include <atomic>
#include <functional>
#include <array>
#include <memory>
#include <string>

#include "lvgl.h"
#include "tab5_icu_page.h"
#include "tab5_ir_remote_page.h"

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
        std::function<void()> radio_stop;
        std::function<void()> radio_next;
        std::function<void()> radio_previous;
        std::function<void(int)> radio_select;
        std::function<int()> station_count;
        std::function<std::string(int)> station_name;
        std::function<int()> radio_level;
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
    };

    explicit Tab5NativeApps(lv_obj_t* screen);
    void SetActions(Actions actions);
    void OpenApps();
    void OpenSettings();
    void OpenRadio();
    void OpenNes();
    void OpenIr();
    void OpenIcu(int mode = -1, const std::string& external_result = "");
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
    void SetMusicLyric(const char* title, const char* artist, const char* line);
    void SetMusicLyricLine(const char* line);
    void SetMusicLyricsWindow(const char* title, const char* artist, const char* previous,
                              const char* current, const char* next);
    void ClearMusicLyrics();
    void Tick();
    bool IsVisible() const;
    bool IsSettingsVisible() const;

private:
    static constexpr int kRows = 6;
    static constexpr int kWaveBars = 40;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* home_page_ = nullptr;
    lv_obj_t* settings_page_ = nullptr;
    lv_obj_t* radio_page_ = nullptr;
    lv_obj_t* game_page_ = nullptr;
    std::unique_ptr<Tab5IcuPage> icu_page_;
    std::unique_ptr<Tab5IrRemotePage> ir_page_;
    lv_obj_t* game_canvas_ = nullptr;
    lv_obj_t* game_status_ = nullptr;
    lv_obj_t* game_select_panel_ = nullptr;
    lv_obj_t* game_play_panel_ = nullptr;
    lv_obj_t* game_rom_label_ = nullptr;
    lv_obj_t* game_rom_rows_[8] = {};
    lv_obj_t* game_rom_names_[8] = {};
    lv_img_dsc_t game_img_{};
    uint16_t* game_src_pixels_ = nullptr;
    // Triple buffer: emu writes, LVGL displays, one slot always free.
    // Prevents Load access fault from LVGL reading while the emu scales.
    uint16_t* game_scaled_[3] = {};
    int game_write_idx_ = 0;
    int game_display_idx_ = -1;
    std::atomic<bool> game_playing_ui_{false};
    std::atomic<bool> game_stop_ui_pending_{false};
    bool game_pad_resync_ = false;
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
    const lv_font_t* music_font_ = nullptr;
    lv_obj_t* radio_play_label_ = nullptr;
    lv_obj_t* station_rows_[kRows] = {};
    lv_obj_t* station_names_[kRows] = {};
    lv_obj_t* station_page_label_ = nullptr;
    lv_obj_t* radio_wave_ = nullptr;
    std::array<uint8_t, kWaveBars> wave_heights_{};
    std::array<lv_color_t, kWaveBars> wave_colors_{};
    std::string current_station_name_;
    std::string current_radio_state_key_;
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
    void Show(lv_obj_t* page);
    void BuildHome();
    void BuildSettings();
    void BuildRadio();
    void BuildGame();
    void RefreshGameRoms();
    void ShowGameSelect();
    void ShowGamePlay();
    void UpdateWave();
    static void DrawWave(lv_event_t* event);
    static void DrawNes(lv_event_t* event);
};
