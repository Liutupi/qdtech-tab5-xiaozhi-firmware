#include "application.h"
#include "button.h"
#include "display/lcd_display.h"
#if CONFIG_QDTECH_TAB5_NATIVE_UI
#include "assets.h"
#include "icu_calculators.h"
#include "lvgl_font.h"
#include "tab5_home_hub.h"
#include "tab5_muse_inbox.h"
#include "tab5_music_lyrics.h"
#include "tab5_music_track_gate.h"
#include "tab5_native_display.h"
#include "tab5_sd_assets.h"
#include "tab5_vision_service.h"
#else
#include "desktop_ui.h"
#endif
#include "esp_cam_sensor_xclk.h"
#include "esp_lcd_ili9881c.h"
#include "esp_lcd_st7121.h"
#include "esp_lcd_st7123.h"

// config.h declares panel initialization tables using the driver types above.
#include "config.h"
#include "esp_video.h"
#include "esp_video_init.h"
#include "ir_service.h"
#include "ir_store.h"
#include "mcp_server.h"
#include "radio_service.h"
#include "tab5_audio_codec.h"
#include "tab5_sd.h"
#include "tab5_ota.h"
#include "tab5_memdiag.h"
#include "fc_emulator_service.h"
#include "settings.h"
#include "tab5_nes_video.h"
#include "usb_gamepad_host.h"
#if !CONFIG_QDTECH_TAB5_NATIVE_UI
#include "firmware_update_service.h"
#include "photo_service.h"
#include "podcast_service.h"
#include "time_weather_service.h"
#endif
#include "websocket_control_server.h"
#include <esp_sntp.h>
#include <cJSON.h>
#include <lwip/sockets.h>
#include <unistd.h>
#include <nvs.h>
#include <nvs_flash.h>
#include "wifi_board.h"
#include "wifi_manager.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_system.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include "esp_check.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_touch_st7123.h"
#include "esp_ldo_regulator.h"
#include "esp_lvgl_port.h"
#include "i2c_device.h"

#define TAG "QdtechTab5Board"

#define AUDIO_CODEC_ES8388_ADDR ES8388_CODEC_DEFAULT_ADDR
#define LCD_MIPI_DSI_PHY_PWR_LDO_CHAN       3  // LDO_VO3 is connected to VDD_MIPI_DPHY
#define LCD_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV 2500
#define ST712X_TOUCH_I2C_ADDRESS 0x55

enum class St712xPanel {
    kSt7121,
    kSt7123,
};

// PI4IO registers
#define PI4IO_REG_CHIP_RESET 0x01
#define PI4IO_REG_IO_DIR     0x03
#define PI4IO_REG_OUT_SET    0x05
#define PI4IO_REG_OUT_H_IM   0x07
#define PI4IO_REG_IN_DEF_STA 0x09
#define PI4IO_REG_PULL_EN    0x0B
#define PI4IO_REG_PULL_SEL   0x0D
#define PI4IO_REG_IN_STA     0x0F
#define PI4IO_REG_INT_MASK   0x11
#define PI4IO_REG_IRQ_STA    0x13

// Bit manipulation macros
#define setbit(x, bit)  ((x) |= (1U << (bit)))
#define clrbit(x, bit)  ((x) &= ~(1U << (bit)))

class Pi4ioe1 : public I2cDevice {
public:
    Pi4ioe1(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        WriteReg(PI4IO_REG_CHIP_RESET, 0xFF);
        uint8_t data = ReadReg(PI4IO_REG_CHIP_RESET);
        // ST712x units require LCD_RST to be released as an input with pull-up.
        WriteReg(PI4IO_REG_IO_DIR, 0b01101111);      // 0: input 1: output
        WriteReg(PI4IO_REG_OUT_H_IM, 0b00000000);    // 使用到的引脚关闭 High-Impedance
        WriteReg(PI4IO_REG_PULL_SEL, 0b01111111);    // pull up/down select, 0 down, 1 up
        WriteReg(PI4IO_REG_PULL_EN, 0b01111111);     // pull up/down enable, 0 disable, 1 enable
        WriteReg(PI4IO_REG_IN_DEF_STA, 0b10000000);  // P1, P7 默认高电平
        WriteReg(PI4IO_REG_INT_MASK, 0b01111111);    // P7 中断使能 0 enable, 1 disable
        WriteReg(PI4IO_REG_OUT_SET, 0b01110110);     // Output Port Register P1(SPK_EN), P2(EXT5V_EN), P4(LCD_RST), P5(TP_RST), P6(CAM)RST 输出高电平
    }

    uint8_t ReadOutSet() { return ReadReg(PI4IO_REG_OUT_SET); }
    void WriteOutSet(uint8_t value) { WriteReg(PI4IO_REG_OUT_SET, value); }

    void SetLcdResetAsserted(bool asserted) {
        uint8_t direction = ReadReg(PI4IO_REG_IO_DIR);
        if (asserted) {
            uint8_t output = ReadReg(PI4IO_REG_OUT_SET);
            clrbit(output, 4);
            WriteReg(PI4IO_REG_OUT_SET, output);
            setbit(direction, 4);
            WriteReg(PI4IO_REG_IO_DIR, direction);
        } else {
            uint8_t pull_select = ReadReg(PI4IO_REG_PULL_SEL);
            uint8_t pull_enable = ReadReg(PI4IO_REG_PULL_EN);
            setbit(pull_select, 4);
            setbit(pull_enable, 4);
            WriteReg(PI4IO_REG_PULL_SEL, pull_select);
            WriteReg(PI4IO_REG_PULL_EN, pull_enable);
            clrbit(direction, 4);
            WriteReg(PI4IO_REG_IO_DIR, direction);
        }
    }
};

class Pi4ioe2 : public I2cDevice {
public:
    Pi4ioe2(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        WriteReg(PI4IO_REG_CHIP_RESET, 0xFF);
        uint8_t data = ReadReg(PI4IO_REG_CHIP_RESET);
        WriteReg(PI4IO_REG_IO_DIR, 0b10111001);      // 0: input 1: output
        WriteReg(PI4IO_REG_OUT_H_IM, 0b00000110);    // 使用到的引脚关闭 High-Impedance
        WriteReg(PI4IO_REG_PULL_SEL, 0b10111001);    // pull up/down select, 0 down, 1 up
        WriteReg(PI4IO_REG_PULL_EN, 0b11111001);     // pull up/down enable, 0 disable, 1 enable
        WriteReg(PI4IO_REG_IN_DEF_STA, 0b01000000);  // P6 默认高电平
        WriteReg(PI4IO_REG_INT_MASK, 0b10111111);    // P6 中断使能 0 enable, 1 disable
        WriteReg(PI4IO_REG_OUT_SET, 0b10001001);     // Output Port Register P0(WLAN_PWR_EN), P3(USB5V_EN), P7(CHG_EN) 输出高电平
    }

    uint8_t ReadOutSet() { return ReadReg(PI4IO_REG_OUT_SET); }
    void WriteOutSet(uint8_t value) { WriteReg(PI4IO_REG_OUT_SET, value); }
};

#if !CONFIG_QDTECH_TAB5_NATIVE_UI
class QdtechTab5Display : public MipiLcdDisplay {
    static constexpr int kLogicalWidth = 480;
    static constexpr int kLogicalHeight = 320;
    static constexpr int kPanelWidth = 720;
    static constexpr int kViewportHeight = 1080;
    static constexpr int kViewportTop = 100;
    uint16_t* scaled_frame_ = nullptr;
    DesktopUI desktop_ui_;

    static int CeilDiv(int value, int divisor) {
        return (value + divisor - 1) / divisor;
    }

    static void FlushScaled(lv_display_t* display, const lv_area_t* area,
                            uint8_t* color_map) {
        auto* self = static_cast<QdtechTab5Display*>(lv_display_get_user_data(display));
        if (!self || !self->scaled_frame_ || !area || !color_map) {
            lv_display_flush_ready(display);
            return;
        }
        const int px0 = CeilDiv((kLogicalHeight - 1 - area->y2) * kPanelWidth,
                                kLogicalHeight);
        const int px1 = CeilDiv((kLogicalHeight - area->y1) * kPanelWidth,
                                kLogicalHeight);
        const int py0 = kViewportTop + CeilDiv(area->x1 * kViewportHeight,
                                               kLogicalWidth);
        const int py1 = kViewportTop + CeilDiv((area->x2 + 1) * kViewportHeight,
                                               kLogicalWidth);
        const int output_width = px1 - px0;
        const int source_width = area->x2 - area->x1 + 1;
        if (output_width <= 0 || py1 <= py0 || px0 < 0 || px1 > kPanelWidth ||
            py0 < kViewportTop || py1 > kViewportTop + kViewportHeight) {
            lv_display_flush_ready(display);
            return;
        }
        const auto* pixels = reinterpret_cast<const uint16_t*>(color_map);
        for (int py = py0; py < py1; ++py) {
            const int lx = (py - kViewportTop) * kLogicalWidth / kViewportHeight;
            uint16_t* row = self->scaled_frame_ + (py - py0) * output_width;
            for (int px = px0; px < px1; ++px) {
                const int ly = kLogicalHeight - 1 - px * kLogicalHeight / kPanelWidth;
                row[px - px0] = pixels[(ly - area->y1) * source_width + lx - area->x1];
            }
        }
        // The esp_lvgl_port DSI completion callback calls flush_ready after
        // the panel has consumed this persistent PSRAM buffer.
        if (esp_lcd_panel_draw_bitmap(self->panel_, px0, py0, px1, py1,
                                      self->scaled_frame_) != ESP_OK) {
            lv_display_flush_ready(display);
        }
    }

public:
    QdtechTab5Display(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                      int width, int height, int offset_x, int offset_y, bool mirror_x,
                      bool mirror_y, bool swap_xy)
        : MipiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                         mirror_x, mirror_y, swap_xy) {
        // A 480x320 logical display preserves the entire original desktop.
        // Scale by 2.25 to 1080x720, rotate into the portrait MIPI panel,
        // and leave 100-pixel bars at the two landscape sides.
        scaled_frame_ = static_cast<uint16_t*>(heap_caps_malloc(
            kPanelWidth * kViewportHeight * sizeof(uint16_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (scaled_frame_) {
            lv_display_set_user_data(display_, this);
            lv_display_set_resolution(display_, kLogicalWidth, kLogicalHeight);
            lv_display_set_flush_cb(display_, FlushScaled);
        } else {
            ESP_LOGE(TAG, "No PSRAM for 480x320 desktop scaling; using native landscape");
            lv_display_set_rotation(display_, LV_DISPLAY_ROTATION_270);
        }
    }

    ~QdtechTab5Display() { heap_caps_free(scaled_frame_); }
    bool IsScaled() const { return scaled_frame_ != nullptr; }

    void SetupUI() override {
        MipiLcdDisplay::SetupUI();
        DisplayLockGuard lock(this);
        if (container_) {
            lv_obj_clear_flag(container_, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
        }
        if (top_bar_) lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        if (status_bar_) lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        if (bottom_bar_) lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        desktop_ui_.Create();
    }

    void SetChatMessage(const char* role, const char* content) override {
        MipiLcdDisplay::SetChatMessage(role, content);
        DisplayLockGuard lock(this);
        desktop_ui_.SetXiaozhiState(role, content, nullptr);
    }

    void SetEmotion(const char* emotion) override {
        MipiLcdDisplay::SetEmotion(emotion);
        DisplayLockGuard lock(this);
        desktop_ui_.SetXiaozhiEmotion(emotion);
    }

    DesktopUI* GetDesktopUI() { return &desktop_ui_; }
};
#endif

// Parked when a USB gamepad connects so USB endpoints can allocate.
static EspVideo* g_tab5_camera = nullptr;

class QdtechTab5Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    LcdDisplay* display_;
    EspVideo* camera_ = nullptr;
    RadioService radio_service_;
    WebSocketControlServer* ws_control_server_ = nullptr;
#if CONFIG_QDTECH_TAB5_NATIVE_UI
    Tab5VisionService vision_service_;
    std::mutex native_radio_mutex_;
    std::atomic<bool> native_radio_ready_{false};
    std::atomic<uint32_t> music_request_generation_{0};
    std::mutex music_track_mutex_;
    tab5_music_track_gate::CurrentTrack music_track_;
    bool native_tools_registered_ = false;
    std::atomic<bool> music_udp_started_{false};
    struct MusicUdpRequest {
        std::string title;
        std::string artist;
        std::string url;
        std::string lyrics;
        std::string song_id;
    };
    std::mutex music_udp_mutex_;
    MusicUdpRequest music_udp_pending_;
    bool music_udp_has_pending_ = false;
    bool music_udp_dispatch_scheduled_ = false;
    // Continuous (private FM / daily recommendation) playback: when a song started with
    // continuous=true ends naturally, Tab5 asks the assistant for the next song itself.
    // The NAS cannot reach Tab5 directly (different subnet / NAT), so the request must
    // originate on the Tab5 and go through the XiaoZhi cloud like a spoken command.
    std::atomic<bool> music_continuous_{false};
    // Sticky: true while the user is in a continuous (daily recommendation) session,
    // so the player's "下一首" button can skip to the next recommended song.
    std::atomic<bool> music_continuous_session_{false};
    // Next-song request state. A request is "pending" from the moment the previous song
    // ended until a new play_url arrives (music_play_count_ changes) or we give up. While
    // pending, the request is re-sent when the assistant turn ends without a song, and the
    // song that does arrive keeps the session continuous even if the model forgot the flag.
    std::atomic<bool> music_next_pending_{false};
    std::atomic<uint32_t> music_play_count_{0};
    uint32_t music_next_base_count_ = 0;
    int music_next_attempts_ = 0;
    int64_t music_next_started_us_ = 0;
    int64_t music_next_last_attempt_us_ = 0;
    esp_timer_handle_t music_next_timer_ = nullptr;
    esp_timer_handle_t ask_song_timer_ = nullptr;
    // Muse 电台 playback session (main task only): plays an episode's tracks in order.
    // Each track is resolved by the NAS (full-length NetEase URL) and played like a
    // daily-recommendation song; a natural end advances to the next track.
    bool podcast_session_ = false;
    int podcast_episode_ = 0;
    int podcast_index_ = 0;
    int podcast_count_ = 0;
    int podcast_misses_ = 0;
    uint32_t podcast_request_seq_ = 0;
    uint32_t podcast_generation_ = 0;

    void SetPodcastStatus(const char* line) {
        if (display_)
            static_cast<QdtechTab5Display*>(display_)->SetMusicInfo("NABO 电台", "", line);
    }

    void StartPodcastTrack(int index) {
        if (!podcast_session_)
            return;
        if (index >= podcast_count_) {
            podcast_session_ = false;
            SetPodcastStatus("本期歌单播完了");
            return;
        }
        podcast_index_ = index;
        const uint32_t seq = ++podcast_request_seq_;
        ReplaceMusicSource([this] {
            EndContinuousSession();
            if (native_radio_ready_.load())
                radio_service_.Stop();
        });
        char line[48];
        std::snprintf(line, sizeof(line), "正在获取第 %d/%d 首…", index + 1, podcast_count_);
        SetPodcastStatus(line);
        ESP_LOGI("Tab5Podcast", "episode %d track %d/%d", podcast_episode_, index + 1,
                 podcast_count_);
        tab5_muse::Inbox::GetInstance().ResolveTrack(
            podcast_episode_, index, [this, seq](const tab5_muse::ResolvedTrack& track) {
                Application::GetInstance().Schedule(
                    [this, seq, track] { OnPodcastTrack(seq, track); });
            });
    }

    void OnPodcastTrack(uint32_t seq, const tab5_muse::ResolvedTrack& track) {
        if (!podcast_session_ || seq != podcast_request_seq_)
            return;  // stopped, or a newer tap replaced this request
        std::string result = "Music URL was NOT started: song unavailable.";
        if (track.ok)
            result = PlayMusicRequest(track.title, track.artist, track.url, "", track.song_id,
                                      false);
        if (result.rfind("Music URL was NOT started", 0) == 0) {
            ESP_LOGW("Tab5Podcast", "track %d unavailable: %s", track.index + 1, result.c_str());
            if (++podcast_misses_ >= 3 || podcast_index_ + 1 >= podcast_count_) {
                podcast_session_ = false;
                SetPodcastStatus("暂时找不到这些歌，请稍后再试");
                return;
            }
            StartPodcastTrack(podcast_index_ + 1);  // skip a song NetEase cannot play
            return;
        }
        podcast_misses_ = 0;
        podcast_generation_ = music_request_generation_.load();
    }
    static constexpr const char* kMusicNextCommand = "继续播放下一首每日推荐";
    static constexpr int kMusicNextMaxAttempts = 3;
    static constexpr int64_t kMusicNextAnswerWaitUs = 40LL * 1000 * 1000;
    static constexpr int64_t kMusicNextGiveUpUs = 150LL * 1000 * 1000;

    static void MusicNextTimerCb(void* arg) {
        auto* board = static_cast<QdtechTab5Board*>(arg);
        Application::GetInstance().Schedule([board] { board->MusicNextTick(); });
    }

    void ArmMusicNextTimer(int delay_ms) {
        if (!music_next_timer_)
            return;
        esp_timer_stop(music_next_timer_);
        esp_timer_start_once(music_next_timer_, uint64_t(delay_ms) * 1000);
    }

    void SetNextSongStatus(const char* line) {
        if (display_)
            static_cast<QdtechTab5Display*>(display_)->SetMusicInfo("每日推荐", "", line);
    }

    // Runs on the main task. Drives one next-song request to completion.
    void MusicNextTick() {
        if (!music_next_pending_.load())
            return;
        if (music_play_count_.load() != music_next_base_count_) {
            music_next_pending_.store(false);  // a song arrived
            return;
        }
        auto& app = Application::GetInstance();
        const auto state = app.GetDeviceState();
        const int64_t now = esp_timer_get_time();
        const int64_t since_attempt = now - music_next_last_attempt_us_;
        // Wait for the assistant's answer, but not when the turn is already over (back to
        // idle) without a song: a successful answer bumps music_play_count_ before that.
        const bool waiting_answer =
            music_next_attempts_ > 0 && since_attempt < kMusicNextAnswerWaitUs &&
            !(state == kDeviceStateIdle && since_attempt > 12LL * 1000 * 1000);
        if (now - music_next_started_us_ > kMusicNextGiveUpUs ||
            (music_next_attempts_ >= kMusicNextMaxAttempts && !waiting_answer)) {
            ESP_LOGW(TAG, "continuous music: no next song after %d request(s), giving up",
                     music_next_attempts_);
            music_next_pending_.store(false);
            music_continuous_session_.store(false);
            SetNextSongStatus("没取到下一首，可说“继续播放每日推荐”");
            return;
        }
        if (waiting_answer) {
            ArmMusicNextTimer(2000);
            return;
        }
        if (music_next_attempts_ > 0 && state == kDeviceStateListening) {
            // The assistant answered without starting a song and is now just listening:
            // close this turn so the request can be sent again.
            ESP_LOGW(TAG, "continuous music: turn ended without a song, closing it to retry");
            app.ToggleChatState();
            ArmMusicNextTimer(1500);
            return;
        }
        if (state == kDeviceStateIdle && app.InvokeTextCommand(kMusicNextCommand)) {
            ++music_next_attempts_;
            music_next_last_attempt_us_ = now;
            ESP_LOGI(TAG, "continuous music: requested next song (attempt %d)",
                     music_next_attempts_);
            SetNextSongStatus(music_next_attempts_ == 1 ? "正在获取下一首…"
                                                        : "正在重新获取下一首…");
        }
        ArmMusicNextTimer(2000);
    }

    void EndContinuousSession() {
        music_continuous_.store(false);
        music_continuous_session_.store(false);
        music_next_pending_.store(false);
        if (music_next_timer_)
            esp_timer_stop(music_next_timer_);
    }

    void OnMusicEndedNaturally() {
        // The radio callback runs on the decoder task while submission_mutex_
        // still protects this event's source stream generation. Do not wait for
        // music_track_mutex_ here; the main task checks the token later.
        const uint32_t stream_generation = radio_service_.GetStreamGeneration();
        const uint32_t generation = music_request_generation_.load();
        const uint32_t play_count = music_play_count_.load();
        // RequestNextSong updates the display and timer. Keep both off the
        // decoder task, and discard this event if Stop/Next or a new song won.
        Application::GetInstance().Schedule([this, stream_generation, generation, play_count] {
            bool next_podcast = false;
            {
                std::lock_guard<std::mutex> guard(music_track_mutex_);
                if (radio_service_.GetStreamGeneration() != stream_generation ||
                    music_request_generation_.load() != generation ||
                    !music_track_.IsCurrent(generation) || music_play_count_.load() != play_count)
                    return;
                // A Muse 电台 track that played to the end: continue with the episode.
                next_podcast = podcast_session_ && generation == podcast_generation_;
                if (!next_podcast) {
                    if (!music_continuous_session_.load() || !music_continuous_.exchange(false))
                        return;
                    RequestNextSong(800);
                }
            }
            // StartPodcastTrack takes music_track_mutex_ itself.
            if (next_podcast)
                StartPodcastTrack(podcast_index_ + 1);
        });
    }

    // All source replacements use the same lock as StartMusicNow. Otherwise a
    // Stop/Next can run while StartMusicNow waits for LVGL, and its older URL
    // can be submitted after the newer command.
    template <typename Action>
    void ReplaceMusicSource(Action&& action) {
        std::lock_guard<std::mutex> guard(music_track_mutex_);
        ++music_request_generation_;
        action();
    }

    void RequestNextSong(int delay_ms) {
        if (!music_next_timer_) {
            esp_timer_create_args_t args = {};
            args.callback = &QdtechTab5Board::MusicNextTimerCb;
            args.arg = this;
            args.dispatch_method = ESP_TIMER_TASK;
            args.name = "music_next";
            if (esp_timer_create(&args, &music_next_timer_) != ESP_OK) {
                music_next_timer_ = nullptr;
                return;
            }
        }
        music_next_base_count_ = music_play_count_.load();
        music_next_attempts_ = 0;
        music_next_started_us_ = esp_timer_get_time();
        music_next_last_attempt_us_ = 0;
        music_next_pending_.store(true);
        music_continuous_session_.store(true);
        // Give the stream task time to release the speaker / external-audio mode.
        ArmMusicNextTimer(delay_ms);
        SetNextSongStatus("正在获取下一首…");
    }

    // Called under music_track_mutex_ for every play_url (MCP or UDP).
    bool NoteMusicPlayRequest(bool continuous) {
        if (music_next_pending_.load())
            continuous = true;  // the answer to our own next-song request
        music_next_pending_.store(false);
        music_play_count_.fetch_add(1);
        music_continuous_.store(continuous);
        music_continuous_session_.store(continuous);
        return continuous;
    }

    struct MusicLookupRequest {
        QdtechTab5Board* board;
        uint32_t generation;
        std::string title;
        std::string artist;
        std::string url;
        std::string song_id;
    };

    bool ApplyLegacyMusicLyricLine(const std::string& title, const std::string& artist,
                                   const std::string& line) {
        std::lock_guard<std::mutex> guard(music_track_mutex_);
        if (!music_track_.Accepts(music_request_generation_.load(), title, artist))
            return false;
        // The track title and artist are set by play_url. An older lyric packet
        // must never be able to replace them, even if its line is accepted.
        static_cast<QdtechTab5Display*>(display_)->SetMusicInfo("", "", line.c_str());
        return true;
    }

    bool ApplyMusicLyricsForTrack(uint32_t generation, const std::string& title,
                                  const std::string& artist, const std::string& lyrics) {
        std::lock_guard<std::mutex> guard(music_track_mutex_);
        if (music_request_generation_.load() != generation ||
            !music_track_.Accepts(generation, title, artist))
            return false;
        return static_cast<QdtechTab5Display*>(display_)->SetMusicLyrics(lyrics.c_str(),
                                                                         title.c_str());
    }

    bool ApplyMusicLyricsFromTool(const std::string& title, const std::string& artist,
                                  const std::string& lyrics) {
        return ApplyMusicLyricsForTrack(music_request_generation_.load(), title, artist, lyrics);
    }

    std::string StartMusicNow(uint32_t generation, const std::string& title,
                              const std::string& artist, const std::string& url,
                              const std::string& lyrics) {
        std::lock_guard<std::mutex> guard(music_track_mutex_);
        if (music_request_generation_.load() != generation)
            return "Music URL was NOT started: superseded by a newer request.";
        auto* display = static_cast<QdtechTab5Display*>(display_);
        music_track_.Begin(generation, title, artist);
        display->BeginMusicTrack(title.c_str(), artist.c_str());
        display->SetMusicInfo(title.c_str(), artist.c_str(), "正在连接音源…");
        const auto result = radio_service_.PlayUrlFromTool(title, artist, url);
        if (result.rfind("Music URL was NOT started", 0) == 0) {
            music_track_.InvalidateIfCurrent(generation);
            display->StopMusicTrack();
            return result;
        }
        if (!lyrics.empty())
            display->SetMusicLyrics(lyrics.c_str(), title.c_str());
        return result;
    }

    // Lyrics are looked up while the song is already playing: the display keeps its own
    // playback clock, so timed LRC that arrives a moment later still lines up.
    static void MusicLookupTask(void* arg) {
        {
            std::unique_ptr<MusicLookupRequest> request(static_cast<MusicLookupRequest*>(arg));
            // Let the MP3 stream connect first so both don't fight over the network.
            vTaskDelay(pdMS_TO_TICKS(1200));
            auto* board = request->board;
            const auto generation = request->generation;
            if (board->music_request_generation_.load() == generation) {
                auto lyrics =
                    tab5_music_lyrics::Lookup(request->title, request->artist, request->song_id);
                if (!lyrics.empty()) {
                    Application::GetInstance().Schedule(
                        [board, generation, title = std::move(request->title),
                         artist = std::move(request->artist), lyrics = std::move(lyrics)] {
                            if (board->music_request_generation_.load() != generation)
                                return;
                            board->ApplyMusicLyricsForTrack(generation, title, artist, lyrics);
                        });
                }
            }
        }
        // Created WithCaps: plain vTaskDelete would leak the TCB and PSRAM stack every song.
        vTaskDeleteWithCaps(nullptr);
    }

    std::string PlayMusicRequest(const std::string& title_value, const std::string& artist,
                                 const std::string& url, const std::string& lyrics,
                                 const std::string& song_id, bool continuous) {
        EnsureNativeRadio();
        const auto title = title_value.empty() ? std::string("Music URL") : title_value;
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
            return std::string("Music URL was NOT started: invalid HTTP(S) URL.");
        uint32_t generation;
        {
            std::lock_guard<std::mutex> guard(music_track_mutex_);
            NoteMusicPlayRequest(continuous);
            generation = ++music_request_generation_;
        }
        tab5_lrc::Document supplied_lyrics;
        if (tab5_lrc::Parse(lyrics, supplied_lyrics) && supplied_lyrics.timed)
            return StartMusicNow(generation, title, artist, url, lyrics);
        auto result = StartMusicNow(generation, title, artist, url, "");
        if (result.rfind("Music URL was NOT started", 0) == 0)
            return result;
        auto* request = new (std::nothrow)
            MusicLookupRequest{this, generation, title, artist, url, song_id};
        TaskHandle_t task = nullptr;
        const BaseType_t created =
            request ? xTaskCreatePinnedToCoreWithCaps(MusicLookupTask, "music_lyrics", 8192,
                                                      request, 2, &task, 0,
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                    : pdFAIL;
        if (created != pdPASS)
            delete request;
        return std::string(
            "Song is playing on Tab5; lyrics are fetched automatically. No spoken follow-up "
            "is needed.");
    }

    // ---- LAN UDP control (NAS NetEase service pushes the next song here when the cloud
    // MCP session is closed).  Packet: {"type":"play_url","title","artist","url","lyrics_json"}.
    static std::string LyricsJsonToLrc(const char* json_text) {
        std::string lrc;
        if (!json_text || !*json_text)
            return lrc;
        cJSON* root = cJSON_Parse(json_text);
        if (!root)
            return lrc;
        cJSON* lines = cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "lines");
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, lines) {
            cJSON* time_ms = cJSON_GetObjectItem(item, "time_ms");
            cJSON* text = cJSON_GetObjectItem(item, "text");
            if (!cJSON_IsNumber(time_ms) || !cJSON_IsString(text) || !text->valuestring[0])
                continue;
            const long ms = static_cast<long>(time_ms->valuedouble);
            char stamp[24];
            snprintf(stamp, sizeof(stamp), "[%02ld:%02ld.%02ld]", ms / 60000, (ms / 1000) % 60,
                     (ms % 1000) / 10);
            if (lrc.size() + strlen(stamp) + strlen(text->valuestring) + 1 > 15000)
                break;
            lrc += stamp;
            lrc += text->valuestring;
            lrc += '\n';
        }
        cJSON_Delete(root);
        return lrc;
    }

    // Keep one latest UDP request plus at most one small callback in the main queue.
    // Process only one song per callback so a busy NAS cannot monopolize the main loop.
    void DispatchLatestMusicUdp() {
        MusicUdpRequest request;
        {
            std::lock_guard<std::mutex> lock(music_udp_mutex_);
            if (!music_udp_has_pending_) {
                music_udp_dispatch_scheduled_ = false;
                return;
            }
            request = std::move(music_udp_pending_);
            music_udp_has_pending_ = false;
        }

        PlayMusicRequest(request.title, request.artist, request.url, request.lyrics,
                         request.song_id, false);

        bool schedule_next;
        {
            std::lock_guard<std::mutex> lock(music_udp_mutex_);
            schedule_next = music_udp_has_pending_;
            if (!schedule_next)
                music_udp_dispatch_scheduled_ = false;
        }
        if (schedule_next)
            Application::GetInstance().Schedule([this] { DispatchLatestMusicUdp(); });
    }

    static void MusicUdpTask(void* arg) {
        auto* board = static_cast<QdtechTab5Board*>(arg);
        constexpr size_t kBufferSize = 24 * 1024;
        constexpr size_t kMaxTitleBytes = 256;
        constexpr size_t kMaxArtistBytes = 256;
        constexpr size_t kMaxUrlBytes = 2048;
        char* buffer = static_cast<char*>(heap_caps_malloc(kBufferSize, MALLOC_CAP_SPIRAM));
        if (!buffer) {
            ESP_LOGE("Tab5Udp", "no memory for UDP buffer");
            vTaskDeleteWithCaps(nullptr);
            return;
        }
        for (;;) {
            int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
            sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            addr.sin_port = htons(45678);
            if (sock < 0 || bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                if (sock >= 0)
                    close(sock);
                vTaskDelay(pdMS_TO_TICKS(5000));  // Wi-Fi not up yet
                continue;
            }
            ESP_LOGI("Tab5Udp", "listening on UDP 45678");
            for (;;) {
                const int len = recvfrom(sock, buffer, kBufferSize - 1, 0, nullptr, nullptr);
                if (len <= 0)
                    break;
                // The setup AP can be reached by nearby devices; local music
                // control is available only after joining the configured Wi-Fi.
                auto& wifi = WifiManager::GetInstance();
                if (!wifi.IsConnected() || wifi.IsConfigMode())
                    continue;
                buffer[len] = 0;
                cJSON* root = cJSON_Parse(buffer);
                if (!root)
                    continue;
                cJSON* type = cJSON_GetObjectItem(root, "type");
                cJSON* url = cJSON_GetObjectItem(root, "url");
                if (cJSON_IsString(type) && strcmp(type->valuestring, "play_url") == 0 &&
                    cJSON_IsString(url)) {
                    cJSON* title = cJSON_GetObjectItem(root, "title");
                    cJSON* artist = cJSON_GetObjectItem(root, "artist");
                    cJSON* lyrics_json = cJSON_GetObjectItem(root, "lyrics_json");
                    cJSON* song_id = cJSON_GetObjectItem(root, "song_id");
                    const char* title_text = cJSON_IsString(title) ? title->valuestring : "";
                    const char* artist_text = cJSON_IsString(artist) ? artist->valuestring : "";
                    const char* url_text = url->valuestring ? url->valuestring : "";
                    // Reject oversized fields instead of cutting through a UTF-8 character.
                    if (strlen(title_text) > kMaxTitleBytes ||
                        strlen(artist_text) > kMaxArtistBytes || strlen(url_text) > kMaxUrlBytes) {
                        ESP_LOGW("Tab5Udp", "oversized play_url packet rejected");
                        cJSON_Delete(root);
                        continue;
                    }
                    if (strncmp(url_text, "http://", 7) != 0 &&
                        strncmp(url_text, "https://", 8) != 0) {
                        ESP_LOGW("Tab5Udp", "empty or non-HTTP(S) play_url rejected");
                        cJSON_Delete(root);
                        continue;
                    }
                    // Invalid optional IDs are ignored; playback and supplied lyrics still work.
                    const char* song_id_text =
                        cJSON_IsString(song_id) ? song_id->valuestring : nullptr;
                    std::string valid_song_id;
                    if (song_id_text) {
                        const size_t id_len = strnlen(song_id_text, 19);
                        if (id_len >= 1 && id_len <= 18 &&
                            std::all_of(song_id_text, song_id_text + id_len,
                                        [](char ch) { return ch >= '0' && ch <= '9'; })) {
                            valid_song_id.assign(song_id_text, id_len);
                        } else {
                            ESP_LOGW("Tab5Udp", "invalid song_id ignored");
                        }
                    }
                    MusicUdpRequest request{
                        title_text, artist_text, url_text, {}, std::move(valid_song_id)};
                    request.lyrics = LyricsJsonToLrc(
                        cJSON_IsString(lyrics_json) ? lyrics_json->valuestring : nullptr);
                    ESP_LOGI("Tab5Udp", "play_url title=%s lyrics=%uB", request.title.c_str(),
                             static_cast<unsigned>(request.lyrics.size()));
                    bool schedule_dispatch = false;
                    {
                        std::lock_guard<std::mutex> lock(board->music_udp_mutex_);
                        board->music_udp_pending_ = std::move(request);
                        board->music_udp_has_pending_ = true;
                        if (!board->music_udp_dispatch_scheduled_) {
                            board->music_udp_dispatch_scheduled_ = true;
                            schedule_dispatch = true;
                        }
                    }
                    if (schedule_dispatch) {
                        Application::GetInstance().Schedule(
                            [board] { board->DispatchLatestMusicUdp(); });
                    }
                }
                // Per-line lyric packets are ignored: Tab5 shows its own timed lyrics.
                cJSON_Delete(root);
            }
            close(sock);
        }
    }

    void StartMusicUdpListener() {
        if (music_udp_started_.exchange(true))
            return;
        TaskHandle_t task = nullptr;
        if (xTaskCreatePinnedToCoreWithCaps(MusicUdpTask, "music_udp", 6144, this, 2, &task, 0,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
            music_udp_started_.store(false);
    }

    // Large resources kept on the SD card (flash assets partition is full). Currently the
    // Noto common CJK text font: downloaded once, then loaded into PSRAM at every boot.
    static void StartSdAssetsTask(QdtechTab5Board* board) {
        xTaskCreatePinnedToCoreWithCaps(
            [](void* arg) {
                auto* self = static_cast<QdtechTab5Board*>(arg);
                // Runs on a PSRAM stack: must never touch flash (no NVS / mmap / OTA APIs).
                const auto& font = tab5_sd_assets::TextFont();
                if (!tab5_sd_assets::Present(font)) {
                    // Wait for Wi-Fi (up to 10 min), then fetch it once.
                    for (int i = 0; i < 300 && !WifiManager::GetInstance().IsConnected(); ++i)
                        vTaskDelay(pdMS_TO_TICKS(2000));
                    vTaskDelay(pdMS_TO_TICKS(15000));  // let the voice session settle first
                    if (!tab5_sd_assets::Ensure(font)) {
                        ESP_LOGW(TAG, "text font not available; using the built-in basic font");
                        vTaskDelete(nullptr);
                        return;
                    }
                }
                size_t loaded = 0;
                void* data = tab5_sd_assets::LoadToPsram(font, &loaded);
                if (data) {
                    // Kept for the lifetime of the firmware: LVGL references the glyph data.
                    auto text_font = std::make_shared<LvglCBinFont>(data);
                    if (text_font->font() && self->display_ && self->display_->SetTextFont(text_font))
                        ESP_LOGI(TAG, "SD text font loaded (%u bytes)", unsigned(loaded));
                    else
                        ESP_LOGW(TAG, "SD text font rejected");
                }
                vTaskDelete(nullptr);
            },
            "sd_assets", 8192, board, 2, nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    void EnsureNativeRadio() {
        std::lock_guard<std::mutex> guard(native_radio_mutex_);
        if (native_radio_ready_.load())
            return;
        auto* native_display = static_cast<QdtechTab5Display*>(display_);
        radio_service_.Start(nullptr, [this, native_display](const char* station, const char* state,
                                                             const char* meta) {
            native_display->PostRadioState(station, state, meta);
            if (state && meta && std::strcmp(state, "Stopped") == 0 &&
                std::strstr(meta, "Music ended"))
                OnMusicEndedNaturally();
        });
        native_radio_ready_.store(radio_service_.IsStarted());
        if (native_radio_ready_.load()) {
            native_display->PostRadioState(
                radio_service_.GetStationName(radio_service_.GetCurrentIndex()), "Ready",
                "Tap Play");
        }
    }

    void RegisterNativeTools() {
        StartMusicUdpListener();
        if (native_tools_registered_)
            return;
        native_tools_registered_ = true;
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.radio.get_status",
                    "Get the current internet radio station and playback state.", PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
                        EnsureNativeRadio();
                        return radio_service_.GetStatusJson();
                    });
        mcp.AddTool("self.radio.play",
                    "When the user asks to listen to radio or broadcast, start internet radio and "
                    "show the radio screen. Optionally choose a station by name.",
                    PropertyList({Property("station", kPropertyTypeString, std::string(""))}),
                    [this](const PropertyList& properties) -> ReturnValue {
                        const auto station = properties["station"].value<std::string>();
                        ReplaceMusicSource([this, &station] {
                            EnsureNativeRadio();
                            if (!station.empty())
                                radio_service_.SelectStation(station);
                            radio_service_.Play();
                            static_cast<QdtechTab5Display*>(display_)->ShowRadioPage();
                        });
                        return true;
                    });
        mcp.AddTool("self.radio.stop", "Stop internet radio playback.", PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
                        // While Tab5 itself asked for the next song, a stop from the model is
                        // just its "stop before play" habit: keep the session alive.
                        ReplaceMusicSource([this] {
                            if (!music_next_pending_.load())
                                EndContinuousSession();
                            if (native_radio_ready_.load())
                                radio_service_.Stop();
                        });
                        return true;
                    });
        mcp.AddTool("self.nes.status",
                    "Get NES emulator and USB gamepad status.", PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
                        char buf[160];
                        snprintf(buf, sizeof(buf),
                                 "{\"gamepad\":%s,\"nes_mask\":%u,\"sd\":%s}",
                                 UsbGamepadConnected() ? "true" : "false",
                                 unsigned(UsbGamepadNesMask()),
                                 Tab5SdReady() ? "true" : "false");
                        return std::string(buf);
                    });
        mcp.AddTool("self.nes.start",
                    "Start the NES (红白机) emulator. ROMs must be .nes files on the SD card under "
                    "/sdcard/nes. Use a USB gamepad (e.g. SN30 Pro) to play.",
                    PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
                        ReplaceMusicSource([this] {
                            radio_service_.Stop();
                            fc_emulator_service_.SetActive(true);
                            fc_emulator_service_.PlayPause();
                        });
                        return UsbGamepadConnected()
                                   ? std::string("NES started. USB gamepad connected.")
                                   : std::string(
                                         "NES started. Plug a USB gamepad (SN30 Pro D-input) into "
                                         "the Tab5 USB port to play.");
                    });
        mcp.AddTool("self.nes.stop", "Stop the NES emulator.", PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
                        fc_emulator_service_.SetActive(false);
                        fc_emulator_service_.Stop();
                        return true;
                    });
        mcp.AddTool("self.radio.next", "Play the next internet radio station.", PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
                        ReplaceMusicSource([this] {
                            EnsureNativeRadio();
                            radio_service_.Next();
                            static_cast<QdtechTab5Display*>(display_)->ShowRadioPage();
                        });
                        return true;
                    });
        mcp.AddTool("self.radio.previous", "Play the previous internet radio station.",
                    PropertyList(), [this](const PropertyList&) -> ReturnValue {
                        ReplaceMusicSource([this] {
                            EnsureNativeRadio();
                            radio_service_.Prev();
                            static_cast<QdtechTab5Display*>(display_)->ShowRadioPage();
                        });
                        return true;
                    });
        mcp.AddTool(
            "self.daily.set_cards",
            "Push up to 3 daily digest cards into the large Xiaozhi conversation panel "
            "(idle). Use for curated briefs such as medical news. Bodies should be one or "
            "two short sentences. The small daily card keeps quote/history/festival.",
            PropertyList({Property("title0", kPropertyTypeString, std::string("")),
                          Property("body0", kPropertyTypeString, std::string("")),
                          Property("title1", kPropertyTypeString, std::string("")),
                          Property("body1", kPropertyTypeString, std::string("")),
                          Property("title2", kPropertyTypeString, std::string("")),
                          Property("body2", kPropertyTypeString, std::string(""))}),
            [this](const PropertyList& p) -> ReturnValue {
                const auto t0 = p["title0"].value<std::string>();
                const auto t1 = p["title1"].value<std::string>();
                const auto t2 = p["title2"].value<std::string>();
                const auto b0 = p["body0"].value<std::string>();
                const auto b1 = p["body1"].value<std::string>();
                const auto b2 = p["body2"].value<std::string>();
                const char* titles[3] = {t0.c_str(), t1.c_str(), t2.c_str()};
                const char* bodies[3] = {b0.c_str(), b1.c_str(), b2.c_str()};
                auto valid = [](const std::string& text, size_t max_characters) {
                    if (text.empty() || text.size() > max_characters * 4)
                        return false;
                    size_t characters = 0;
                    for (unsigned char ch : text) {
                        if (ch < 0x20 || ch == 0x7f)
                            return false;
                        if ((ch & 0xc0) != 0x80)
                            ++characters;
                    }
                    return characters <= max_characters;
                };
                const std::string* card_titles[3] = {&t0, &t1, &t2};
                const std::string* card_bodies[3] = {&b0, &b1, &b2};
                unsigned cards = 0;
                for (int i = 0; i < 3; ++i) {
                    if (card_titles[i]->empty() && card_bodies[i]->empty())
                        continue;
                    if (!valid(*card_titles[i], 14) || !valid(*card_bodies[i], 36))
                        return std::string("Invalid daily card text or length");
                    ++cards;
                }
                if (cards == 0)
                    return std::string("At least one daily card is required");
                if (!display_)
                    return std::string("Display not ready");
                static_cast<QdtechTab5Display*>(display_)->SetDailyCards(titles, bodies);
                return std::string("Daily cards updated");
            });
        mcp.AddTool(
            "self.ir.list",
            "List the infrared devices (TV, air conditioner, fan, ...) the user has set up on the "
            "Tab5 IR remote, with the keys that have been learned. Call before self.ir.send when "
            "unsure of the exact names.",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                ir::Store::GetInstance().Load();
                return ir::Store::GetInstance().SummaryJson();
            });
        mcp.AddTool(
            "self.ir.send",
            "Send a learned infrared key, e.g. device=空调 key=开机, device=电视 key=音量+. Names are "
            "matched loosely (打开/关闭 map to 开机/关机/电源). Only learned keys can be sent; if it "
            "fails, tell the user to learn that key on the Tab5 红外遥控 page.",
            PropertyList({Property("device", kPropertyTypeString, std::string("")),
                          Property("key", kPropertyTypeString)}),
            [](const PropertyList& p) -> ReturnValue {
                auto& store = ir::Store::GetInstance();
                store.Load();
                int device_id = -1, key_id = -1;
                std::string resolved;
                if (!store.Find(p["device"].value<std::string>(), p["key"].value<std::string>(),
                                &device_id, &key_id, &resolved))
                    return std::string("No learned IR key matches. Learned keys: ") + store.SummaryJson();
                std::vector<uint16_t> t;
                uint32_t carrier = 38000;
                int repeat = 1;
                store.GetTimings(device_id, key_id, &t, &carrier, &repeat);
                const auto cfg = store.GetConfig();
                const bool ok =
                    IrService::GetInstance().Send(cfg.tx_gpio, t.data(), t.size(), cfg.active_high, carrier, repeat);
                return ok ? "Sent IR key " + resolved : std::string("IR transmit failed");
            });
        // Wiring/debug helpers: user-only so they do not count against the assistant's tool limit.
        mcp.AddUserOnlyTool(
            "self.ir.scan",
            "Probe IR receiver wiring: count edges on candidate GPIOs while a remote button is pressed.",
            PropertyList({Property("seconds", kPropertyTypeInteger, 5, 1, 20)}),
            [](const PropertyList& p) -> ReturnValue {
                static const int kGpios[] = {14, 15, 16, 17, 18, 19, 20, 21, 24, 25, 33, 34,
                                             35, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54};
                return IrService::GetInstance().ScanGpios(kGpios, sizeof(kGpios) / sizeof(kGpios[0]),
                                                          p["seconds"].value<int>());
            });
        mcp.AddUserOnlyTool(
            "self.ir.learn",
            "Learn one IR code on the receiver pin and return its timings_us.",
            PropertyList({Property("timeout_ms", kPropertyTypeInteger, 8000, 1000, 30000)}),
            [](const PropertyList& p) -> ReturnValue {
                return IrService::GetInstance().LearnFrame(ir::Store::GetInstance().GetConfig().rx_gpio,
                                                           p["timeout_ms"].value<int>());
            });
        mcp.AddUserOnlyTool(
            "self.ir.send_raw",
            "Send raw mark/space microsecond timings (comma separated, starting with a mark).",
            PropertyList({Property("timings_us", kPropertyTypeString),
                          Property("carrier_hz", kPropertyTypeInteger, 38000, 20000, 60000)}),
            [](const PropertyList& p) -> ReturnValue {
                const auto text = p["timings_us"].value<std::string>();
                std::vector<uint16_t> us;
                for (size_t pos = 0; pos < text.size() && us.size() < 1500;) {
                    const size_t comma = text.find(',', pos);
                    const int v = atoi(text.substr(pos, comma - pos).c_str());
                    if (v > 0)
                        us.push_back(uint16_t(std::min(v, 65535)));
                    if (comma == std::string::npos)
                        break;
                    pos = comma + 1;
                }
                if (us.empty())
                    return std::string("no timings");
                const auto cfg = ir::Store::GetInstance().GetConfig();
                return IrService::GetInstance().Send(cfg.tx_gpio, us.data(), us.size(), cfg.active_high,
                                                     uint32_t(p["carrier_hz"].value<int>()), 1)
                           ? std::string("IR sent")
                           : std::string("IR send failed");
            });
        mcp.AddUserOnlyTool(
            "self.ir.loopback",
            "Transmit a probe frame while listening on the receiver; reports which LED polarity was heard.",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                const auto cfg = ir::Store::GetInstance().GetConfig();
                const bool high = IrService::GetInstance().LoopbackHeard(cfg.tx_gpio, cfg.rx_gpio, true);
                const bool low = !high && IrService::GetInstance().LoopbackHeard(cfg.tx_gpio, cfg.rx_gpio, false);
                return std::string("{\"active_high_heard\":") + (high ? "true" : "false") +
                       ",\"active_low_heard\":" + (low ? "true" : "false") + "}";
            });
        mcp.AddTool(
            "self.music.play_url",
            "Play a direct HTTP(S) MP3 song URL on Tab5. Pass only the exact title, artist, "
            "url and numeric song_id from the NAS/NetEase result, plus continuous. NEVER pass "
            "lyrics or lyrics_json, even if the NAS result contains them: Tab5 fetches the "
            "lyrics itself, and copying them makes the song start very late. Call this right "
            "away without stopping playback first and without a spoken introduction. Set "
            "continuous to true when the NAS play_url_arguments include continuous=true "
            "(private FM / daily recommendation); Tab5 then fetches the next song by itself.",
            PropertyList({Property("title", kPropertyTypeString, std::string("Music")),
                          Property("artist", kPropertyTypeString, std::string("")),
                          Property("url", kPropertyTypeString),
                          Property("song_id", kPropertyTypeString, std::string("")),
                          Property("continuous", kPropertyTypeBoolean, false)}),
            [this](const PropertyList& p) -> ReturnValue {
                return PlayMusicRequest(
                    p["title"].value<std::string>(), p["artist"].value<std::string>(),
                    p["url"].value<std::string>(), std::string(), p["song_id"].value<std::string>(),
                    p["continuous"].value<bool>());
            });
        mcp.AddTool(
            "self.muse.inbox",
            "Read the latest messages that the user's Muse agent pushed to the Tab5 inbox "
            "(newest first). Use when the user asks what Muse sent / 有什么推送. Summarize them "
            "briefly in speech; they are also shown in the Tab5 Muse app.",
            PropertyList({Property("limit", kPropertyTypeInteger, 3, 1, 10)}),
            [](const PropertyList& p) -> ReturnValue {
                auto& inbox = tab5_muse::Inbox::GetInstance();
                const auto snapshot = inbox.Current();
                if (!snapshot.ever_ok)
                    return std::string("Muse inbox relay on the NAS is not reachable yet (") +
                           snapshot.host + ").";
                if (snapshot.messages.empty())
                    return std::string("The Muse inbox is empty.");
                const int limit = p["limit"].value<int>();
                std::string out;
                int n = 0;
                for (const auto& m : snapshot.messages) {
                    if (n++ >= limit)
                        break;
                    out += "[" + m.time + "] " + m.title + "\n" + m.body + "\n\n";
                }
                inbox.MarkAllSeen();
                return out;
            });
        // 米家中控: Home Assistant on the NAS through the relay. Requests are queued; the
        // result shows on the 米家中控 page, so these tools return at once.
        mcp.AddTool(
            "self.home.list",
            "List the user's smart-home combined scenes (e.g. 卧室电脑, 家庭影院) and devices "
            "(lights, switches, air conditioners, TV, speakers) by room with their current state. "
            "Call before self.home.scene or self.home.control when unsure of the name.",
            PropertyList(), [](const PropertyList&) -> ReturnValue {
                auto& hub = tab5_home::Hub::GetInstance();
                const auto status = hub.Current();
                hub.RequestRefresh();
                if (!status.catalog)
                    return std::string("Smart home not available yet: ") +
                           (status.message.empty() ? "waiting for the NAS" : status.message);
                std::string out = "Scenes:";
                for (const auto& sc : status.catalog->scenes)
                    out += " " + std::string(sc.name.data(), sc.name.size()) + ";";
                out += "\nDevices:";
                for (const auto& d : status.catalog->devices) {
                    if (d.kind() == tab5_home::Kind::kScene || d.kind() == tab5_home::Kind::kOther)
                        continue;
                    out += "\n" + std::string(d.area.data(), d.area.size()) + " " +
                           std::string(d.label.data(), d.label.size()) + ": " +
                           (d.available() ? std::string(d.state.data(), d.state.size()) : "offline");
                    if (d.kind() == tab5_home::Kind::kClimate && d.target > -999)
                        out += " target " + std::to_string(int(d.target)) + "C";
                }
                return out;
            });
        mcp.AddTool(
            "self.home.scene",
            "Run one of the user's combined smart-home scenes, e.g. 打开卧室电脑 (PC + screen "
            "speaker + monitor light) or 关闭家庭影院. name: the scene name or the user's words.",
            PropertyList({Property("name", kPropertyTypeString), Property("on", kPropertyTypeBoolean, true)}),
            [](const PropertyList& p) -> ReturnValue {
                auto& hub = tab5_home::Hub::GetInstance();
                const auto status = hub.Current();
                const std::string name = p["name"].value<std::string>();
                const int i = status.catalog ? tab5_home::MatchScene(*status.catalog, name) : -1;
                if (i < 0)
                    return "No scene matches '" + name + "'. Use self.home.list to see the scenes.";
                const auto& sc = status.catalog->scenes[i];
                const bool on = p["on"].value<bool>();
                if ((on && !sc.has_on) || (!on && !sc.has_off))
                    return std::string("That scene cannot be turned ") + (on ? "on." : "off.");
                if (!hub.RunScene(std::string(sc.id.data(), sc.id.size()), on))
                    return std::string("Too many requests, try again in a moment.");
                return std::string("Requested: ") + (on ? "打开" : "关闭") +
                       std::string(sc.name.data(), sc.name.size());
            });
        mcp.AddTool(
            "self.home.control",
            "Control one smart-home device. target: the device in the user's words, with the room "
            "when said (e.g. 书房的灯, 卧室空调, 客厅电视). action: on, off, or temperature "
            "(air conditioner, value in °C 16-32).",
            PropertyList({Property("target", kPropertyTypeString), Property("action", kPropertyTypeString),
                          Property("value", kPropertyTypeInteger, 26, 16, 32)}),
            [](const PropertyList& p) -> ReturnValue {
                auto& hub = tab5_home::Hub::GetInstance();
                const auto status = hub.Current();
                const std::string target = p["target"].value<std::string>();
                const std::string action = p["action"].value<std::string>();
                if (action != "on" && action != "off" && action != "temperature")
                    return std::string("action must be on, off or temperature.");
                const int i = status.catalog ? tab5_home::MatchDevice(*status.catalog, target) : -1;
                if (i < 0)
                    return "No device matches '" + target + "'. Use self.home.list to see the devices.";
                const auto& d = status.catalog->devices[i];
                if (!d.available())
                    return std::string(d.label.data(), d.label.size()) + " is offline.";
                if (action == "temperature" && d.kind() != tab5_home::Kind::kClimate)
                    return std::string("Only air conditioners take a temperature.");
                const std::string value = action == "temperature" ? std::to_string(p["value"].value<int>()) : "";
                if (!hub.Control(std::string(d.id.data(), d.id.size()), action, value))
                    return std::string("Too many requests, try again in a moment.");
                return "Requested: " + std::string(d.area.data(), d.area.size()) +
                       std::string(d.label.data(), d.label.size()) + " " + action + (value.empty() ? "" : " " + value);
            });
        mcp.AddUserOnlyTool("self.muse.set_url",
                    "Set the public Muse relay URL (the MCP URL .../mcp/<token> or the inbox URL "
                    ".../inbox/<token>). Used by the setup script; do not call from conversation.",
                    PropertyList({Property("url", kPropertyTypeString)}),
                    [](const PropertyList& p) -> ReturnValue {
                        return tab5_muse::Inbox::GetInstance().SetUrl(p["url"].value<std::string>());
                    });
        mcp.AddUserOnlyTool("self.muse.set_host",
                    "Set the NAS address (host:port) of the Muse inbox relay. Default "
                    "192.168.3.200:8787. Only when the user asks to change it.",
                    PropertyList({Property("host", kPropertyTypeString)}),
                    [](const PropertyList& p) -> ReturnValue {
                        tab5_muse::Inbox::GetInstance().SetHost(p["host"].value<std::string>());
                        return true;
                    });
        mcp.AddTool("self.music.get_status",
                    "Get direct-song playback state: stopped, playing, ended, or unavailable.",
                    PropertyList(), [this](const PropertyList&) -> ReturnValue {
                        EnsureNativeRadio();
                        return radio_service_.GetMusicStatusJson();
                    });
        mcp.AddTool(
            "self.music.set_lyrics",
            "Display complete timed LRC lyrics for the currently playing Tab5 song. Retrieve real "
            "LRC from the connected NAS/NetEase MCP first, then pass its raw [mm:ss.xx] lines as "
            "lyrics. Call after self.music.play_url when lyrics were not included there. Do not "
            "fabricate lyrics. Include the exact title and artist from play_url so delayed lyrics "
            "cannot overwrite another song.",
            PropertyList({Property("title", kPropertyTypeString, std::string("")),
                          Property("artist", kPropertyTypeString, std::string("")),
                          Property("lyrics", kPropertyTypeString)}),
            [this](const PropertyList& p) -> ReturnValue {
                const auto title = p["title"].value<std::string>();
                const auto artist = p["artist"].value<std::string>();
                const auto lyrics = p["lyrics"].value<std::string>();
                return ApplyMusicLyricsFromTool(title, artist, lyrics)
                           ? "Lyrics shown and synchronized on Tab5."
                           : "Lyrics not shown: supply the current song title and valid LRC (max "
                             "16 KB).";
            });
        // The NAS NetEase MCP pushes lyric lines under this name (older protocol).
        mcp.AddTool("self.music.show_lyric",
                    "Show the current lyric line on Tab5. Include the exact title and artist from "
                    "play_url; delayed lines for another song are ignored.",
                    PropertyList({Property("title", kPropertyTypeString, std::string("")),
                                  Property("artist", kPropertyTypeString, std::string("")),
                                  Property("line", kPropertyTypeString, std::string("")),
                                  Property("udp_port", kPropertyTypeInteger, 0, 0, 65535)}),
                    [this](const PropertyList& p) -> ReturnValue {
                        return ApplyLegacyMusicLyricLine(p["title"].value<std::string>(),
                                                         p["artist"].value<std::string>(),
                                                         p["line"].value<std::string>());
                    });
        mcp.AddTool("self.music.set_lyric",
                    "Show the current lyric line on Tab5 after music starts. Include the exact "
                    "title and artist from play_url; delayed lines for another song are ignored.",
                    PropertyList({Property("title", kPropertyTypeString, std::string("")),
                                  Property("artist", kPropertyTypeString, std::string("")),
                                  Property("line", kPropertyTypeString, std::string(""))}),
                    [this](const PropertyList& p) -> ReturnValue {
                        return ApplyLegacyMusicLyricLine(p["title"].value<std::string>(),
                                                         p["artist"].value<std::string>(),
                                                         p["line"].value<std::string>());
                    });
        mcp.AddTool("self.music.stop", "Stop song playback.", PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
                        ReplaceMusicSource([this] {
                            if (music_next_pending_.load()) {
                                // Answering our own next-song request: nothing to stop yet.
                                if (native_radio_ready_.load())
                                    radio_service_.Stop();
                                return;
                            }
                            EndContinuousSession();
                            if (native_radio_ready_.load())
                                radio_service_.Stop();
                            static_cast<QdtechTab5Display*>(display_)->StopMusicTrack();
                            static_cast<QdtechTab5Display*>(display_)->SetMusicInfo(
                                "已停止", "", "点歌或收听电台");
                        });
                        return true;
                    });
        auto show_icu = [this](int mode, const icu::Result& result) -> ReturnValue {
            static_cast<QdtechTab5Display*>(display_)->ShowIcuPage(mode, result.text, result.ok);
            return result.text;
        };
        mcp.AddTool("self.icu.open",
            "Open the Tab5 ICU calculator. module is egfr, uacr, oxygen, blood_gas or pump. Ask for measured values and units before calculating.",
            PropertyList({Property("module", kPropertyTypeString, std::string(""))}),
            [this](const PropertyList& p) -> ReturnValue {
                const auto name = p["module"].value<std::string>();
                const int mode = name == "egfr" ? 0 : name == "uacr" ? 1 :
                    name == "oxygen" ? 2 : name == "blood_gas" ? 3 :
                    name == "pump" ? 4 : -1;
                static_cast<QdtechTab5Display*>(display_)->ShowIcuPage(mode);
                return true;
            });
        mcp.AddTool("self.icu.egfr",
            "Calculate adult 2021 CKD-EPI creatinine eGFR. age_years and serum_creatinine_umol_l are decimal strings; sex must be male or female. Not reliable for unstable creatinine or AKI.",
            PropertyList({Property("age_years", kPropertyTypeString),
                          Property("sex", kPropertyTypeString),
                          Property("serum_creatinine_umol_l", kPropertyTypeString)}),
            [show_icu](const PropertyList& p) -> ReturnValue {
                double age, scr;
                const auto sex = p["sex"].value<std::string>();
                if (!icu::ParseDecimal(p["age_years"].value<std::string>(), age) ||
                    !icu::ParseDecimal(p["serum_creatinine_umol_l"].value<std::string>(), scr) ||
                    (sex != "male" && sex != "female")) return std::string("请核对年龄、性别 male/female 与血肌酐 μmol/L。");
                return show_icu(0, icu::Egfr(age, sex == "female", scr));
            });
        mcp.AddTool("self.icu.uacr",
            "Calculate urine albumin-to-creatinine ratio from the same urine sample. Input decimal strings in mg/L and mmol/L.",
            PropertyList({Property("urine_albumin_mg_l", kPropertyTypeString),
                          Property("urine_creatinine_mmol_l", kPropertyTypeString)}),
            [show_icu](const PropertyList& p) -> ReturnValue {
                double albumin, creatinine;
                if (!icu::ParseDecimal(p["urine_albumin_mg_l"].value<std::string>(), albumin) ||
                    !icu::ParseDecimal(p["urine_creatinine_mmol_l"].value<std::string>(), creatinine))
                    return std::string("请核对尿白蛋白 mg/L 与尿肌酐 mmol/L。");
                return show_icu(1, icu::Uacr(albumin, creatinine));
            });
        mcp.AddTool("self.icu.oxygen",
            "Calculate PaO2/FiO2 and, optionally, formal oxygenation index OI. Inputs are decimal strings: FiO2 percent 21-100, PaO2 mmHg, optional mean airway pressure cmH2O. No ARDS diagnosis.",
            PropertyList({Property("fio2_percent", kPropertyTypeString),
                          Property("pao2_mmhg", kPropertyTypeString),
                          Property("mean_airway_pressure_cmh2o", kPropertyTypeString, std::string(""))}),
            [show_icu](const PropertyList& p) -> ReturnValue {
                double fio2, pao2, map = 0;
                const auto optional = p["mean_airway_pressure_cmh2o"].value<std::string>();
                if (!icu::ParseDecimal(p["fio2_percent"].value<std::string>(), fio2) ||
                    !icu::ParseDecimal(p["pao2_mmhg"].value<std::string>(), pao2) ||
                    (!optional.empty() && !icu::ParseDecimal(optional, map)))
                    return std::string("请核对 FiO₂ 百分数、PaO₂ mmHg 和平均气道压 cmH₂O。");
                return show_icu(2, icu::Oxygen(fio2, pao2, map));
            });
        mcp.AddTool("self.icu.blood_gas",
            "Display acid-base measurements, anion gap and Winter expected PaCO2 where applicable. Decimal strings: pH, PaCO2 mmHg, HCO3 mmol/L; sodium, chloride mmol/L and albumin g/L optional. No diagnosis.",
            PropertyList({Property("ph", kPropertyTypeString), Property("paco2_mmhg", kPropertyTypeString),
                          Property("hco3_mmol_l", kPropertyTypeString),
                          Property("sodium_mmol_l", kPropertyTypeString, std::string("")),
                          Property("chloride_mmol_l", kPropertyTypeString, std::string("")),
                          Property("albumin_g_l", kPropertyTypeString, std::string(""))}),
            [show_icu](const PropertyList& p) -> ReturnValue {
                double ph, co2, hco3, sodium = 0, chloride = 0, albumin = 0;
                auto parse_optional = [&p](const char* name, double& value) {
                    const auto input = p[name].value<std::string>();
                    return input.empty() || icu::ParseDecimal(input, value);
                };
                if (!icu::ParseDecimal(p["ph"].value<std::string>(), ph) ||
                    !icu::ParseDecimal(p["paco2_mmhg"].value<std::string>(), co2) ||
                    !icu::ParseDecimal(p["hco3_mmol_l"].value<std::string>(), hco3) ||
                    !parse_optional("sodium_mmol_l", sodium) ||
                    !parse_optional("chloride_mmol_l", chloride) ||
                    !parse_optional("albumin_g_l", albumin)) return std::string("请核对血气输入值与单位。");
                return show_icu(3, icu::BloodGas(ph, co2, hco3, sodium, chloride, albumin));
            });
        mcp.AddTool("self.icu.pump",
            "Convert an existing infusion preparation and mL/h pump rate; NEVER recommend a dose. drug must be norepinephrine, epinephrine, metaraminol, dopamine, dobutamine, amiodarone, omeprazole, vasopressin, somatostatin, octreotide, insulin, furosemide, or dexmedetomidine. amount is mg except vasopressin/insulin in U. final_volume_ml is the FINAL total syringe volume, not added diluent; rate_ml_h and optional weight_kg are decimal strings. Verify diluent compatibility separately.",
            PropertyList({Property("drug", kPropertyTypeString), Property("amount", kPropertyTypeString),
                          Property("final_volume_ml", kPropertyTypeString), Property("rate_ml_h", kPropertyTypeString),
                          Property("weight_kg", kPropertyTypeString, std::string(""))}),
            [show_icu](const PropertyList& p) -> ReturnValue {
                const auto drug = p["drug"].value<std::string>();
                const char* names[] = {"norepinephrine", "epinephrine", "metaraminol", "dopamine",
                    "dobutamine", "amiodarone", "omeprazole", "vasopressin", "somatostatin",
                    "octreotide", "insulin", "furosemide", "dexmedetomidine"};
                int index = -1;
                for (int i = 0; i < icu::kDrugCount; ++i) if (drug == names[i]) index = i;
                double amount, volume, rate, weight = 0;
                const auto optional = p["weight_kg"].value<std::string>();
                if (index < 0 || !icu::ParseDecimal(p["amount"].value<std::string>(), amount) ||
                    !icu::ParseDecimal(p["final_volume_ml"].value<std::string>(), volume) ||
                    !icu::ParseDecimal(p["rate_ml_h"].value<std::string>(), rate) ||
                    (!optional.empty() && !icu::ParseDecimal(optional, weight)))
                    return std::string("请核对药物名称、药量、最终总液量 mL、泵速 mL/h 和体重 kg。");
                return show_icu(4, icu::Pump(static_cast<icu::Drug>(index), amount, volume, rate, weight));
            });
    }
#endif
    Pi4ioe1* pi4ioe1_;
    Pi4ioe2* pi4ioe2_;
    esp_lcd_touch_handle_t touch_ = nullptr;
    FcEmulatorService fc_emulator_service_;
#if !CONFIG_QDTECH_TAB5_NATIVE_UI
    TimeWeatherService time_weather_service_;
    PhotoService photo_service_;
    PodcastService podcast_service_;
    bool time_weather_started_ = false;
    bool product_tools_registered_ = false;

    void SetDesktopMusicInfo(const char* title, const char* artist, const char* line) {
        if (!display_ || !lvgl_port_lock(250))
            return;
        static_cast<QdtechTab5Display*>(display_)->GetDesktopUI()->SetMusicLyric(title, artist,
                                                                                 line);
        lvgl_port_unlock();
    }

    void RegisterProductTools() {
        if (product_tools_registered_) return;
        product_tools_registered_ = true;
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.weather.set_location", "Set the weather city and coordinates.",
            PropertyList({Property("city", kPropertyTypeString),
                          Property("latitude", kPropertyTypeString),
                          Property("longitude", kPropertyTypeString)}),
            [this](const PropertyList& p) -> ReturnValue {
                const auto city = p["city"].value<std::string>();
                return time_weather_service_.SetLocation(
                    city, p["latitude"].value<std::string>(),
                    p["longitude"].value<std::string>())
                    ? std::string("Weather location updated to ") + city
                    : std::string("Invalid weather location");
            });
        mcp.AddTool("self.display.qrcode", "Show a URL or text QR code on the Tab5 display.",
            PropertyList({Property("content", kPropertyTypeString),
                          Property("title", kPropertyTypeString, std::string("Scan QR")),
                          Property("hint", kPropertyTypeString, std::string("Tap to close"))}),
            [this](const PropertyList& p) -> ReturnValue {
                const auto content = p["content"].value<std::string>();
                if (content.empty() || content.size() > 700 || !lvgl_port_lock(250)) {
                    return std::string("QR code could not be shown");
                }
                auto* ui = static_cast<QdtechTab5Display*>(display_)->GetDesktopUI();
                bool shown = ui->ShowQrCode(content.c_str(),
                    p["title"].value<std::string>().c_str(),
                    p["hint"].value<std::string>().c_str());
                lvgl_port_unlock();
                return shown ? std::string("QR code shown") : std::string("QR encoding failed");
            });
        mcp.AddTool("self.radio.get_status", "Get the current radio station and playback state.",
            PropertyList(), [this](const PropertyList&) -> ReturnValue {
                return radio_service_.GetStatusJson();
            });
        mcp.AddTool("self.radio.play", "Play radio, optionally selecting a station by name.",
            PropertyList({Property("station", kPropertyTypeString, std::string(""))}),
            [this](const PropertyList& p) -> ReturnValue {
                const auto station = p["station"].value<std::string>();
                if (!station.empty()) radio_service_.SelectStation(station);
                radio_service_.Play();
                return true;
            });
        mcp.AddTool("self.radio.stop", "Stop radio playback.", PropertyList(),
            [this](const PropertyList&) -> ReturnValue { radio_service_.Stop(); return true; });
        mcp.AddTool("self.radio.next", "Play the next radio station.", PropertyList(),
            [this](const PropertyList&) -> ReturnValue { radio_service_.Next(); return true; });
        mcp.AddTool("self.radio.previous", "Play the previous radio station.", PropertyList(),
            [this](const PropertyList&) -> ReturnValue { radio_service_.Prev(); return true; });
        mcp.AddTool("self.music.play_url", "Play a direct HTTP MP3 URL on the Tab5 speaker.",
                    PropertyList({Property("title", kPropertyTypeString, std::string("Music")),
                                  Property("artist", kPropertyTypeString, std::string("")),
                                  Property("url", kPropertyTypeString)}),
                    [this](const PropertyList& p) -> ReturnValue {
                        const auto title = p["title"].value<std::string>();
                        const auto artist = p["artist"].value<std::string>();
                        SetDesktopMusicInfo(title.c_str(), artist.c_str(), "正在连接音源…");
                        return radio_service_.PlayUrlFromTool(title, artist,
                                                              p["url"].value<std::string>());
                    });
        mcp.AddTool(
            "self.music.set_lyric",
            "Show the current lyric line on the Tab5 screen. Call after self.music.play_url.",
            PropertyList({Property("title", kPropertyTypeString, std::string("")),
                          Property("artist", kPropertyTypeString, std::string("")),
                          Property("line", kPropertyTypeString, std::string(""))}),
            [this](const PropertyList& p) -> ReturnValue {
                SetDesktopMusicInfo(p["title"].value<std::string>().c_str(),
                                    p["artist"].value<std::string>().c_str(),
                                    p["line"].value<std::string>().c_str());
                return true;
            });
        mcp.AddTool(
            "self.music.stop", "Stop MP3 URL playback.", PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                if (display_ && lvgl_port_lock(250)) {
                    static_cast<QdtechTab5Display*>(display_)->GetDesktopUI()->ClearMusicLyric();
                    lvgl_port_unlock();
                }
                radio_service_.Stop();
                return true;
            });
    }
#endif
#if !CONFIG_QDTECH_TAB5_NATIVE_UI
    bool desktop_touch_pressed_ = false;
    uint16_t desktop_touch_start_x_ = 0;
    uint16_t desktop_touch_start_y_ = 0;
    uint16_t desktop_touch_last_x_ = 0;
    uint16_t desktop_touch_last_y_ = 0;
    int64_t desktop_touch_started_ms_ = 0;

    void ProcessDesktopTouch(bool pressed, uint16_t x = 0, uint16_t y = 0) {
        auto* desktop = display_ ? static_cast<QdtechTab5Display*>(display_)->GetDesktopUI() : nullptr;
        if (!desktop) return;
        if (pressed) {
            if (!desktop_touch_pressed_) {
                desktop_touch_start_x_ = x;
                desktop_touch_start_y_ = y;
                desktop_touch_started_ms_ = esp_timer_get_time() / 1000;
                desktop_touch_pressed_ = true;
            }
            desktop_touch_last_x_ = x;
            desktop_touch_last_y_ = y;
            desktop->HandleTouchState(x, y, true);
        } else if (desktop_touch_pressed_) {
            desktop_touch_pressed_ = false;
            desktop->HandleTouchState(desktop_touch_last_x_, desktop_touch_last_y_, false);
            desktop->HandleTouchRelease(desktop_touch_start_x_, desktop_touch_start_y_,
                                        desktop_touch_last_x_, desktop_touch_last_y_,
                                        esp_timer_get_time() / 1000 - desktop_touch_started_ms_);
        }
    }
#endif

    void RegisterTouchWithLvgl() {
        if (touch_ == nullptr) {
            ESP_LOGE(TAG, "Touch controller was not initialized");
            return;
        }
        // The LVGL port starts its own task when MipiLcdDisplay is created.
        lvgl_port_lock(0);
        lv_display_t* display = lv_display_get_default();
        lv_indev_t* indev = display ? lv_indev_create() : nullptr;
        if (indev) {
            lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
            lv_indev_set_mode(indev, LV_INDEV_MODE_TIMER);
            lv_indev_set_read_cb(indev, [](lv_indev_t* indev, lv_indev_data_t* data) {
                auto* board = static_cast<QdtechTab5Board*>(lv_indev_get_user_data(indev));
                auto* touch = board ? board->touch_ : nullptr;
                data->state = LV_INDEV_STATE_RELEASED;
                if (!touch || esp_lcd_touch_read_data(touch) != ESP_OK) {
#if !CONFIG_QDTECH_TAB5_NATIVE_UI
                    if (board) board->ProcessDesktopTouch(false);
#endif
                    return;
                }
                uint8_t count = 0;
                esp_lcd_touch_point_data_t point[1] = {};
                if (esp_lcd_touch_get_data(touch, point, &count, 1) == ESP_OK && count) {
#if CONFIG_QDTECH_TAB5_NATIVE_UI
                    // LVGL rotates pointer samples itself when the display is
                    // set to 270 degrees. Pass raw portrait panel coordinates.
                    data->point.x = point[0].x;
                    data->point.y = point[0].y;
                    data->state = LV_INDEV_STATE_PRESSED;
#else
                    const auto* screen = static_cast<QdtechTab5Display*>(board->display_);
                    if (screen && screen->IsScaled()) {
                        if (point[0].y < 100 || point[0].y >= 1180) {
                            board->ProcessDesktopTouch(false);
                            return;
                        }
                        data->point.x = (point[0].y - 100) * 480 / 1080;
                        data->point.y = 319 - point[0].x * 320 / 720;
                    } else {
                        // LVGL applies the display rotation in native mode.
                        data->point.x = point[0].x;
                        data->point.y = point[0].y;
                    }
                    data->state = LV_INDEV_STATE_PRESSED;
                    board->ProcessDesktopTouch(true, data->point.x, data->point.y);
#endif
                } else {
#if !CONFIG_QDTECH_TAB5_NATIVE_UI
                    board->ProcessDesktopTouch(false);
#endif
                }
            });
            lv_indev_set_disp(indev, display);
            lv_indev_set_user_data(indev, this);
        }
        lvgl_port_unlock();
        ESP_LOGI(TAG, "LVGL touch input %s", indev ? "registered" : "registration failed");
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    static esp_err_t bsp_enable_dsi_phy_power() {
        esp_ldo_channel_handle_t ldo_mipi_phy        = NULL;
        esp_ldo_channel_config_t ldo_mipi_phy_config = {
            .chan_id    = LCD_MIPI_DSI_PHY_PWR_LDO_CHAN,
            .voltage_mv = LCD_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
        };
        return esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy);
    }

    [[maybe_unused]] void I2cDetect() {
        uint8_t address;
        printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");
        for (int i = 0; i < 128; i += 16) {
            printf("%02x: ", i);
            for (int j = 0; j < 16; j++) {
                fflush(stdout);
                address       = i + j;
                esp_err_t ret = i2c_master_probe(i2c_bus_, address, pdMS_TO_TICKS(200));
                if (ret == ESP_OK) {
                    printf("%02x ", address);
                } else if (ret == ESP_ERR_TIMEOUT) {
                    printf("UU ");
                } else {
                    printf("-- ");
                }
            }
            printf("\r\n");
        }
    }

    void InitializePi4ioe() {
        ESP_LOGI(TAG, "Init I/O Exapander PI4IOE");
        pi4ioe1_ = new Pi4ioe1(i2c_bus_, 0x43);
        pi4ioe2_ = new Pi4ioe2(i2c_bus_, 0x44);
    }

    void ResetLcdAndTouch() {
        ESP_LOGI(TAG, "Reset LCD and touch via PI4IOE");
        gpio_reset_pin(TOUCH_INT_GPIO);

        uint8_t value = pi4ioe1_->ReadOutSet();
        clrbit(value, 5);  // P5 = TP_RST
        pi4ioe1_->WriteOutSet(value);
        pi4ioe1_->SetLcdResetAsserted(true);
        vTaskDelay(pdMS_TO_TICKS(100));

        setbit(value, 5);
        pi4ioe1_->WriteOutSet(value);
        pi4ioe1_->SetLcdResetAsserted(false);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    static esp_lcd_panel_io_i2c_config_t St712xTouchIoConfig() {
        esp_lcd_panel_io_i2c_config_t config = {};
        config.scl_speed_hz = 100000;
        config.dev_addr = ST712X_TOUCH_I2C_ADDRESS;
        config.control_phase_bytes = 1;
        config.lcd_cmd_bits = 16;
        config.flags.disable_control_phase = 1;
        return config;
    }

    St712xPanel DetectSt712xPanel() {
        esp_lcd_panel_io_handle_t touch_io = nullptr;
        auto touch_io_config = St712xTouchIoConfig();
        esp_err_t ret = esp_lcd_new_panel_io_i2c(i2c_bus_, &touch_io_config, &touch_io);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to create ST712x detection IO (%s); falling back to ST7123",
                     esp_err_to_name(ret));
            return St712xPanel::kSt7123;
        }

        uint8_t firmware_version = 0;
        ret = esp_lcd_panel_io_rx_param(touch_io, 0x0000, &firmware_version,
                                        sizeof(firmware_version));
        esp_lcd_panel_io_del(touch_io);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to read ST712x firmware version (%s); falling back to ST7123",
                     esp_err_to_name(ret));
            return St712xPanel::kSt7123;
        }

        if (firmware_version == 1) {
            ESP_LOGI(TAG, "Detected ST7121 panel (touch firmware version %u)", firmware_version);
            return St712xPanel::kSt7121;
        }
        if (firmware_version != 3) {
            ESP_LOGW(TAG, "Unknown ST712x touch firmware version %u; falling back to ST7123",
                     firmware_version);
        } else {
            ESP_LOGI(TAG, "Detected ST7123 panel (touch firmware version %u)", firmware_version);
        }
        return St712xPanel::kSt7123;
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeGt911TouchPad() {
        ESP_LOGI(TAG, "Init GT911");
 
        /* Initialize Touch Panel */
        ESP_LOGI(TAG, "Initialize touch IO (I2C)");
        const esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH,
            .y_max = DISPLAY_HEIGHT,
            .rst_gpio_num = GPIO_NUM_NC, 
            .int_gpio_num = TOUCH_INT_GPIO, 
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {
            .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, 
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 16,                            
            .flags =
            {
                .disable_control_phase = 1,
            }
	    };
        tp_io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP; // 更改 GT911 地址 
        tp_io_config.scl_speed_hz = 100000;
        esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle);
        esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &touch_);
    }

    void InitializeIli9881cDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGI(TAG, "Turn on the power for MIPI DSI PHY");
        esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
        esp_ldo_channel_config_t ldo_mipi_phy_config = {
            .chan_id = LCD_MIPI_DSI_PHY_PWR_LDO_CHAN,
            .voltage_mv = LCD_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
        };
        ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy));

        ESP_LOGI(TAG, "Install MIPI DSI LCD control panel");
        esp_lcd_dsi_bus_handle_t mipi_dsi_bus;
        esp_lcd_dsi_bus_config_t bus_config = {
            .bus_id = 0,
            .num_data_lanes = 2,
            .lane_bit_rate_mbps = 900, // 900MHz
        };
        ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));

        ESP_LOGI(TAG, "Install panel IO");
        esp_lcd_dbi_io_config_t dbi_config = {
            .virtual_channel = 0,
            .lcd_cmd_bits = 8,
            .lcd_param_bits  = 8,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &panel_io));

        ESP_LOGI(TAG, "Install LCD driver of ili9881c");
        esp_lcd_dpi_panel_config_t dpi_config = {
            .virtual_channel = 0,
            .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
            .dpi_clock_freq_mhz = 60,
            .in_color_format = LCD_COLOR_FMT_RGB565,
            .out_color_format = LCD_COLOR_FMT_RGB565,
            .num_fbs = 2,
            .video_timing = {
                .h_size = DISPLAY_WIDTH,
                .v_size = DISPLAY_HEIGHT,
                .hsync_pulse_width = 40,
                .hsync_back_porch  = 140,
                .hsync_front_porch = 40,
                .vsync_pulse_width = 4,
                .vsync_back_porch  = 20,
                .vsync_front_porch = 20,
            },
        };
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
        dpi_config.flags.use_dma2d = true;
#endif

        ili9881c_vendor_config_t vendor_config = {
            .init_cmds = tab5_lcd_ili9881c_specific_init_code_default,
            .init_cmds_size = sizeof(tab5_lcd_ili9881c_specific_init_code_default) / sizeof(tab5_lcd_ili9881c_specific_init_code_default[0]),
            .mipi_config = {
                .dsi_bus = mipi_dsi_bus,
                .dpi_config = &dpi_config,
                .lane_num = 2,
            },
        };

        esp_lcd_panel_dev_config_t lcd_dev_config = {};
        lcd_dev_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        lcd_dev_config.reset_gpio_num = GPIO_NUM_NC;
        lcd_dev_config.bits_per_pixel = 16;
        lcd_dev_config.vendor_config = &vendor_config;

        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9881c(panel_io, &lcd_dev_config, &panel));
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
        ESP_ERROR_CHECK(esp_lcd_dpi_panel_enable_dma2d(panel));
#endif
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

        display_ = new QdtechTab5Display(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X,
                                      DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeSt712xDisplay(St712xPanel panel_type) {
        const bool is_st7121 = panel_type == St712xPanel::kSt7121;
        const char* panel_name = is_st7121 ? "ST7121" : "ST7123";
        esp_err_t ret = ESP_OK;
        esp_lcd_panel_io_handle_t io = NULL;
        esp_lcd_panel_handle_t disp_panel = NULL;
        esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
        
        // Declare all config structures at the top to avoid goto issues
        // Initialize with memset to avoid any initialization syntax that might confuse the compiler
        esp_lcd_dsi_bus_config_t bus_config;
        esp_lcd_dbi_io_config_t dbi_config;
        esp_lcd_dpi_panel_config_t dpi_config;
        st7121_vendor_config_t st7121_vendor_config;
        st7123_vendor_config_t st7123_vendor_config;
        esp_lcd_panel_dev_config_t lcd_dev_config;
        
        memset(&bus_config, 0, sizeof(bus_config));
        memset(&dbi_config, 0, sizeof(dbi_config));
        memset(&dpi_config, 0, sizeof(dpi_config));
        memset(&st7121_vendor_config, 0, sizeof(st7121_vendor_config));
        memset(&st7123_vendor_config, 0, sizeof(st7123_vendor_config));
        memset(&lcd_dev_config, 0, sizeof(lcd_dev_config));

        ESP_ERROR_CHECK(bsp_enable_dsi_phy_power());

        /* create MIPI DSI bus first, it will initialize the DSI PHY as well */
        bus_config.bus_id = 0;
        bus_config.num_data_lanes = 2;
        bus_config.lane_bit_rate_mbps = 965;
        ret = esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "New DSI bus init failed");
            goto err;
        }

        ESP_LOGI(TAG, "Install MIPI DSI LCD control panel for %s", panel_name);
        // we use DBI interface to send LCD commands and parameters
        dbi_config.virtual_channel = 0;
        dbi_config.lcd_cmd_bits = 8;  // according to the LCD spec
        dbi_config.lcd_param_bits = 8;  // according to the LCD spec
        ret = esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "New panel IO failed");
            goto err;
        }

        ESP_LOGI(TAG, "Install LCD driver of %s", panel_name);
        dpi_config.virtual_channel = 0;
        dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
        dpi_config.dpi_clock_freq_mhz = 70;
        dpi_config.in_color_format = LCD_COLOR_FMT_RGB565;
        dpi_config.out_color_format = LCD_COLOR_FMT_RGB565;
        // Two scan-out buffers so the NES emulator can page-flip on VSYNC
        // (tab5_nes_video). LVGL keeps drawing into whichever one is shown.
        dpi_config.num_fbs = 2;
        dpi_config.video_timing.h_size = 720;
        dpi_config.video_timing.v_size = 1280;
        dpi_config.video_timing.hsync_pulse_width = 2;
        dpi_config.video_timing.hsync_back_porch = 40;
        dpi_config.video_timing.hsync_front_porch = 40;
        dpi_config.video_timing.vsync_pulse_width = is_st7121 ? 20 : 2;
        dpi_config.video_timing.vsync_back_porch = is_st7121 ? 24 : 8;
        dpi_config.video_timing.vsync_front_porch = is_st7121 ? 200 : 220;
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
        dpi_config.flags.use_dma2d = true;
#endif

        lcd_dev_config.reset_gpio_num = GPIO_NUM_NC;
        lcd_dev_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        lcd_dev_config.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
        lcd_dev_config.bits_per_pixel = 24;
        if (is_st7121) {
            st7121_vendor_config.mipi_config.dsi_bus = mipi_dsi_bus;
            st7121_vendor_config.mipi_config.dpi_config = &dpi_config;
            lcd_dev_config.vendor_config = &st7121_vendor_config;
            ret = esp_lcd_new_panel_st7121(io, &lcd_dev_config, &disp_panel);
        } else {
            st7123_vendor_config.init_cmds = st7123_vendor_specific_init_default;
            st7123_vendor_config.init_cmds_size = sizeof(st7123_vendor_specific_init_default) /
                                                  sizeof(st7123_vendor_specific_init_default[0]);
            st7123_vendor_config.mipi_config.dsi_bus = mipi_dsi_bus;
            st7123_vendor_config.mipi_config.dpi_config = &dpi_config;
            st7123_vendor_config.mipi_config.lane_num = 2;
            lcd_dev_config.vendor_config = &st7123_vendor_config;
            ret = esp_lcd_new_panel_st7123(io, &lcd_dev_config, &disp_panel);
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "New LCD panel %s failed", panel_name);
            goto err;
        }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
        ret = esp_lcd_dpi_panel_enable_dma2d(disp_panel);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Enable DPI DMA2D failed");
            goto err;
        }
#endif

        ret = esp_lcd_panel_reset(disp_panel);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LCD panel reset failed");
            goto err;
        }

        ret = esp_lcd_panel_init(disp_panel);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LCD panel init failed");
            goto err;
        }

        ret = esp_lcd_panel_disp_on_off(disp_panel, true);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LCD panel display on failed");
            goto err;
        }

        display_ = new QdtechTab5Display(io, disp_panel, 720, 1280, DISPLAY_OFFSET_X,
                                      DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);

        ESP_LOGI(TAG, "%s display initialized with resolution %dx%d", panel_name, 720, 1280);

        return;

    err:
        if (disp_panel) {
            esp_lcd_panel_del(disp_panel);
        }
        if (io) {
            esp_lcd_panel_io_del(io);
        }
        if (mipi_dsi_bus) {
            esp_lcd_del_dsi_bus(mipi_dsi_bus);
        }
        ESP_ERROR_CHECK(ret);
    }

    void InitializeSt712xTouchPad() {
        ESP_LOGI(TAG, "Init ST712x touch");

        /* Initialize Touch Panel */
        ESP_LOGI(TAG, "Initialize touch IO (I2C)");
        const esp_lcd_touch_config_t tp_cfg = {
            .x_max = 720,
            .y_max = 1280,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = TOUCH_INT_GPIO,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        auto tp_io_config = St712xTouchIoConfig();
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_st7123(tp_io_handle, &tp_cfg, &touch_));
    }

    void InitializeDisplay() {
        ResetLcdAndTouch();

        esp_err_t ret = i2c_master_probe(i2c_bus_, ST712X_TOUCH_I2C_ADDRESS, 200);
        if (ret == ESP_OK) {
            St712xPanel panel_type = DetectSt712xPanel();
            InitializeSt712xDisplay(panel_type);
            InitializeSt712xTouchPad();
        } else {
            ESP_LOGI(TAG, "ST712x not found at 0x%02X (ret=0x%x), using default ILI9881C+GT911",
                     ST712X_TOUCH_I2C_ADDRESS, ret);
            InitializeIli9881cDisplay();
            InitializeGt911TouchPad();
        }
        RegisterTouchWithLvgl();
    }

    void InitializeCamera() {
        esp_cam_sensor_xclk_handle_t xclk_handle = NULL;
        esp_cam_sensor_xclk_config_t cam_xclk_config = {};

#if CONFIG_CAMERA_XCLK_USE_ESP_CLOCK_ROUTER
        if (esp_cam_sensor_xclk_allocate(ESP_CAM_SENSOR_XCLK_ESP_CLOCK_ROUTER, &xclk_handle) == ESP_OK) {
            cam_xclk_config.esp_clock_router_cfg.xclk_pin = CAMERA_MCLK;
            cam_xclk_config.esp_clock_router_cfg.xclk_freq_hz = 12000000; // 12MHz
            (void)esp_cam_sensor_xclk_start(xclk_handle, &cam_xclk_config);
        }
#elif CONFIG_CAMERA_XCLK_USE_LEDC
        if (esp_cam_sensor_xclk_allocate(ESP_CAM_SENSOR_XCLK_LEDC, &xclk_handle) == ESP_OK) {
            cam_xclk_config.ledc_cfg.timer = LEDC_TIMER_0;
            cam_xclk_config.ledc_cfg.clk_cfg = LEDC_AUTO_CLK;
            cam_xclk_config.ledc_cfg.channel = LEDC_CHANNEL_0;
            cam_xclk_config.ledc_cfg.xclk_freq_hz = 12000000; // 12MHz
            cam_xclk_config.ledc_cfg.xclk_pin = CAMERA_MCLK;
            (void)esp_cam_sensor_xclk_start(xclk_handle, &cam_xclk_config);
        }
#endif

        esp_video_init_sccb_config_t sccb_config = {
            .init_sccb = false,
            .i2c_handle = i2c_bus_,
            .freq = 400000,
        };

        esp_video_init_csi_config_t csi_config = {
            .sccb_config = sccb_config,
            .reset_pin = GPIO_NUM_NC,
            .pwdn_pin = GPIO_NUM_NC,
        };

        esp_video_init_config_t video_config = {
            .csi = &csi_config,
        };

#if CONFIG_QDTECH_TAB5_NATIVE_UI
        // Native UI keeps camera sensing active alongside LVGL and audio. Prefer
        // the smaller RGB565 CSI buffers, with RGB24 fallback in EspVideo.
        camera_ = new EspVideo(video_config, EspVideo::PixelFormatPreference::PreferRgb565);
#else
        camera_ = new EspVideo(video_config);
#endif
        g_tab5_camera = camera_;
    }

public:
    QdtechTab5Board() : boot_button_(BOOT_BUTTON_GPIO) {
        // Distinguish freeze-vs-reboot: brownout, panic, task wdt, or power-on.
        const esp_reset_reason_t why = esp_reset_reason();
        ESP_LOGI(TAG, "boot reset_reason=%d (%s) free_sram=%u min_sram=%u", int(why),
                 why == ESP_RST_POWERON   ? "poweron"
                 : why == ESP_RST_EXT     ? "ext"
                 : why == ESP_RST_SW      ? "sw"
                 : why == ESP_RST_PANIC   ? "panic"
                 : why == ESP_RST_INT_WDT ? "int_wdt"
                 : why == ESP_RST_TASK_WDT ? "task_wdt"
                 : why == ESP_RST_WDT     ? "wdt"
                 : why == ESP_RST_DEEPSLEEP ? "deepsleep"
                 : why == ESP_RST_BROWNOUT ? "BROWNOUT"
                 : why == ESP_RST_USB     ? "usb"
                                          : "other",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        InitializeI2c();
        // 调试用的 I2C 全地址扫描每次开机要多花约 6.6 秒（128 次 200ms 超时），默认关闭。
#ifdef QDTECH_TAB5_I2C_SCAN_AT_BOOT
        I2cDetect();
#endif
        InitializePi4ioe();
        Tab5MemDiag::Stage("before display");
        InitializeDisplay();  // Auto-detect and initialize display + touch
        Tab5MemDiag::Stage("after display");
        InitializeCamera();
        Tab5MemDiag::Stage("after camera");
        InitializeButtons();
        Tab5MemDiag::ScheduleReport();
        SetChargeQcEn(true);
        SetChargeEn(true);
        SetUsb5vEn(true);
        SetExt5vEn(true);
        GetBacklight()->RestoreBrightness();
        // USB gamepad starts after Wi-Fi/MQTT: starting it at boot leaves too
        // little internal SRAM for `mqtt_client` task creation.
        // UsbGamepadHostStart() is invoked from StartNetwork().
    }

    // Keep BALANCED Wi-Fi power save while external audio plays. The native-only
    // experiment also keeps it while idle to measure incoming network latency.
    void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level == PowerSaveLevel::LOW_POWER) {
#if CONFIG_QDTECH_TAB5_IDLE_BALANCED_WIFI_EXPERIMENT
            level = PowerSaveLevel::BALANCED;
#else
            if (Application::GetInstance().IsExternalAudioActive())
                level = PowerSaveLevel::BALANCED;
#endif
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

    void StartNetwork() override {
        WifiBoard::StartNetwork();
        // Native UI has no TimeWeatherService; start SNTP here so the clock
        // and daily-card date logic actually run.
        setenv("TZ", "CST-8", 1);
        tzset();
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_setservername(1, "pool.ntp.org");
        esp_sntp_set_sync_interval(10 * 60 * 1000);
        esp_sntp_init();
        ESP_LOGI(TAG, "SNTP started (CST-8)");
        // LAN MCP bridge for OpenClaw / host agents (ws://<ip>:8080/ws).
        if (ws_control_server_ == nullptr) {
            ws_control_server_ = new WebSocketControlServer();
            if (!ws_control_server_->Start(8080)) {
                delete ws_control_server_;
                ws_control_server_ = nullptr;
            }
        }
        // Mount SD + load fallback font OFF the startup path. A slow/failed
        // SDMMC init or lv_binfont_create must never block Initialize()/Run()
        // or the main event loop — that freezes chat, MCP tools and every
        // Schedule() callback while LVGL still paints (looks like "UI alive
        // but dead / flash on tap").
        xTaskCreate(
            [](void* arg) {
#if CONFIG_QDTECH_TAB5_NATIVE_UI
                auto* self = static_cast<QdtechTab5Board*>(arg);
#endif
                // Let ESP-Hosted finish claiming the shared SDMMC host first.
                vTaskDelay(pdMS_TO_TICKS(800));
                if (!Tab5SdReady()) {
                    Tab5SdMountAndRegisterFs();
                }
#if CONFIG_QDTECH_TAB5_NATIVE_UI
                if (self->display_ && Tab5SdReady()) {
                    // lv_binfont_create touches LVGL heap — hold the port lock.
                    lvgl_port_lock(0);
                    static_cast<QdtechTab5Display*>(self->display_)->LoadSdFallbackFont();
                    lvgl_port_unlock();
                }
#endif
                // Learned IR codes: load now (SD mounted or not) so the remote page and the
                // voice tool do not wait for the card later.
                ir::Store::GetInstance().Load();
#if CONFIG_QDTECH_TAB5_NATIVE_UI
                // Assets::GetInstance() may mmap the flash partition, which needs an
                // internal-RAM stack — decide here, before handing off to the PSRAM task.
                if (Tab5SdReady()) {
                    void* ptr = nullptr;
                    size_t size = 0;
                    if (Assets::GetInstance().GetAssetData(tab5_sd_assets::TextFont().name, ptr,
                                                           size))
                        ESP_LOGI(TAG, "text font is in the flash assets; SD copy not needed");
                    else
                        StartSdAssetsTask(self);
                }
#endif
                vTaskDelete(nullptr);
            },
            "tab5_sd_font", 10 * 1024, this, 2, nullptr);
        // Bring USB HID up once Wi-Fi/MQTT have claimed their internal SRAM.
        // UsbLibTask retries install if memory is still tight.
        xTaskCreate(
            [](void* arg) {
                vTaskDelay(pdMS_TO_TICKS(2500));
                UsbGamepadHostStart();
                vTaskDelete(nullptr);
            },
            "tab5_usb_early", 2048, nullptr, 2, nullptr);
#if CONFIG_QDTECH_TAB5_NATIVE_UI
        if (display_) {
            auto* native_display = static_cast<QdtechTab5Display*>(display_);
            Tab5NativeApps::Actions actions;
            actions.reconfigure_wifi = [this] {
                ReplaceMusicSource([this] {
                    if (native_radio_ready_.load())
                        radio_service_.Stop();
                });
                EnterWifiConfigMode();
            };
            actions.wifi_summary = [] {
                auto& wifi = WifiManager::GetInstance();
                if (wifi.IsConfigMode())
                    return std::string("配网热点：") + wifi.GetApSsid() + "  地址：" +
                           wifi.GetApWebUrl();
                if (wifi.IsConnected())
                    return std::string("已连接：") + wifi.GetSsid() + "  " + wifi.GetIpAddress();
                return std::string("网络未连接");
            };
            actions.get_brightness = [this] {
                auto* backlight = GetBacklight();
                return backlight ? int(backlight->brightness()) : 0;
            };
            actions.get_volume = [this] {
                auto* codec = GetAudioCodec();
                return codec ? codec->output_volume() : 0;
            };
            actions.set_brightness = [this](int value) {
                if (auto* backlight = GetBacklight())
                    backlight->SetBrightness(std::clamp(value, 5, 100), true);
            };
            actions.set_volume = [this](int value) {
                if (auto* codec = GetAudioCodec())
                    codec->SetOutputVolume(std::clamp(value, 0, 100));
            };
            actions.start_radio = [this] { EnsureNativeRadio(); };
            actions.radio_play_requested = [this] { return radio_service_.IsPlayRequested(); };
            actions.radio_play_pause = [this] {
                ReplaceMusicSource([this] {
                    EnsureNativeRadio();
                    radio_service_.PlayPause();
                });
            };
            actions.radio_stop = [this] {
                ReplaceMusicSource([this] {
                    podcast_session_ = false;
                    ++podcast_request_seq_;
                    EndContinuousSession();
                    if (native_radio_ready_.load())
                        radio_service_.Stop();
                });
            };
            actions.radio_next = [this] {
                ReplaceMusicSource([this] {
                    if (music_continuous_session_.load()) {
                        music_continuous_.store(false);  // intentional skip, not a natural end
                        if (native_radio_ready_.load())
                            radio_service_.Stop();
                        RequestNextSong(600);
                        return;
                    }
                    EndContinuousSession();
                    EnsureNativeRadio();
                    radio_service_.Next();
                });
            };
            actions.ask_song = [this] {
                // Silence the speaker first so the microphone hears the request,
                // then open a voice turn once the player has released the audio path.
                ReplaceMusicSource([this] {
                    EndContinuousSession();
                    if (native_radio_ready_.load())
                        radio_service_.Stop();
                });
                if (!ask_song_timer_) {
                    esp_timer_create_args_t args = {};
                    args.callback = [](void*) {
                        auto& app = Application::GetInstance();
                        app.Schedule([] {
                            auto& app = Application::GetInstance();
                            if (app.GetDeviceState() == kDeviceStateIdle)
                                app.ToggleChatState();
                        });
                    };
                    args.dispatch_method = ESP_TIMER_TASK;
                    args.name = "ask_song";
                    if (esp_timer_create(&args, &ask_song_timer_) != ESP_OK) {
                        ask_song_timer_ = nullptr;
                        return;
                    }
                }
                esp_timer_stop(ask_song_timer_);
                esp_timer_start_once(ask_song_timer_, 600 * 1000);
            };
            actions.radio_previous = [this] {
                ReplaceMusicSource([this] {
                    EndContinuousSession();
                    EnsureNativeRadio();
                    radio_service_.Prev();
                });
            };
            actions.radio_select = [this](int index) {
                ReplaceMusicSource([this, index] {
                    EndContinuousSession();
                    EnsureNativeRadio();
                    radio_service_.SelectStationIndex(index);
                });
            };
            actions.station_count = [this] {
                // Catalog load does not need the player task.
                return radio_service_.GetStationCount();
            };
            actions.station_name = [this](int index) {
                const char* name = radio_service_.GetStationName(index);
                return std::string(name ? name : "");
            };
            actions.radio_level = [this] {
                return native_radio_ready_.load() ? radio_service_.GetAudioLevel() : 0;
            };
            actions.podcast_position = [this](std::string_view url) {
                return radio_service_.GetPlaybackPosition(url);
            };
            actions.start_nes = [this] {
                ReplaceMusicSource([this] {
                    radio_service_.Stop();
                    // Game mode: free camera + claim audio so vision/MQTT stay off.
                    if (camera_)
                        camera_->PauseStream();
                    Application::GetInstance().SetExternalAudioActive(true);
                    UsbGamepadSetOnConnect([] {
                        if (g_tab5_camera)
                            g_tab5_camera->PauseStream();
                    });
                    // USB host needs contiguous internal SRAM. Start it before the
                    // FC task claims another block; retry is idempotent.
                    UsbGamepadHostStart();
                    fc_emulator_service_.SetActive(true);
                    fc_emulator_service_.PlayPause();
                });
            };
            actions.stop_nes = [this] {
                // Stop the emulator but keep the page/session so the next
                // ROM can be started without a full teardown.
                fc_emulator_service_.Stop();
            };
            actions.nes_play_pause = [this] {
                fc_emulator_service_.SetActive(true);
                fc_emulator_service_.StartSelected();
            };
            actions.nes_next = [this] { fc_emulator_service_.Next(); };
            actions.nes_previous = [this] { fc_emulator_service_.Prev(); };
            actions.nes_status = [this] {
                char buf[160];
                snprintf(buf, sizeof(buf), "手柄: %s  ROM: %s",
                         UsbGamepadConnected() ? "已连接" : "未连接",
                         fc_emulator_service_.SelectedName().c_str());
                static_cast<QdtechTab5Display*>(display_)->SetNesStatus(buf);
            };
            actions.nes_rom_count = [this] { return fc_emulator_service_.RomCount(); };
            actions.nes_rom_name = [this](int i) { return fc_emulator_service_.RomNameAt(i); };
            actions.nes_rom_index = [this] { return fc_emulator_service_.CurrentRomIndex(); };
            actions.firmware_action = [] { Tab5Ota::GetInstance().HandleButton(); };
            actions.podcast_play_url = [this](const std::string& url, const std::string& title) {
                Application::GetInstance().Schedule([this, url, title] {
                    EnsureNativeRadio();
                    uint32_t generation;
                    ReplaceMusicSource([this, &generation] {
                        // A late song lookup must not replace the narrated episode.
                        podcast_session_ = false;
                        ++podcast_request_seq_;
                        EndContinuousSession();
                        NoteMusicPlayRequest(false);
                        generation = music_request_generation_.load();
                    });
                    const auto result = StartMusicNow(generation, title, "NABO 电台", url, "");
                    if (result.rfind("Music URL was NOT started", 0) != 0)
                        ApplyLegacyMusicLyricLine(title, "NABO 电台", "播客播放中");
                    ESP_LOGI("Tab5Podcast", "episode audio: %s", result.c_str());
                });
                return true;
            };
            actions.podcast_play = [this](int episode, int index, int count) {
                if (episode <= 0 || index < 0 || count <= 0 || index >= count)
                    return false;
                Application::GetInstance().Schedule([this, episode, index, count] {
                    podcast_session_ = true;
                    podcast_episode_ = episode;
                    podcast_count_ = count;
                    podcast_misses_ = 0;
                    StartPodcastTrack(index);
                });
                return true;
            };
            actions.muse_refresh = [] { tab5_muse::Inbox::GetInstance().RequestRefresh(); };
            actions.muse_opened = [] { tab5_muse::Inbox::GetInstance().MarkAllSeen(); };
            native_display->SetAppsActions(std::move(actions), [this] {
                ReplaceMusicSource([this] {
                    EnsureNativeRadio();
                    radio_service_.Play();
                });
            });
            Tab5Ota::GetInstance().SetStatusCallback(
                [native_display](const Tab5Ota::Status& status) {
                    native_display->SetFirmwareStatus(status.text, status.button, status.progress,
                                                      status.busy);
                });
            RegisterNativeTools();
            tab5_muse::Inbox::GetInstance().Start(
                [native_display, announced = std::make_shared<std::atomic<int>>(-1)](
                    const tab5_muse::Snapshot& snapshot) {
                    // First successful poll after boot only records the baseline; later
                    // increases of latest_id with unread messages are "new arrivals".
                    const int previous = announced->exchange(snapshot.latest_id);
                    const bool new_arrival =
                        previous >= 0 && snapshot.latest_id > previous && snapshot.Unread() > 0;
                    native_display->SetMuseInbox(snapshot, new_arrival);
                },
                [this](const tab5_muse::MusicCommand& command) {
                    Application::GetInstance().Schedule([this, command] {
                        const auto result =
                            PlayMusicRequest(command.title, command.artist, command.url, "",
                                             command.song_id, command.continuous);
                        ESP_LOGI("Tab5Music", "relay play_url: %s", result.c_str());
                    });
                });
            tab5_home::Hub::GetInstance().Start([native_display](const tab5_home::Status& status) {
                native_display->SetHomeStatus(status);
            });
            native_display->SetPresenceTestAction(
                [this] { vision_service_.RequestPresenceTest(); });
            native_display->SetInteractionAction([this] { vision_service_.NotifyInteraction(); });
            if (camera_)
                vision_service_.Start(camera_, native_display);
            // NES + USB gamepad (SN30 Pro). Frames can later bind to a native page.
            fc_emulator_service_.Start({
                .set_state =
                    [native_display](const char* title, const char* detail, const char*) {
                        char buf[160];
                        snprintf(buf, sizeof(buf), "%s%s%s", title ? title : "",
                                 (detail && *detail) ? " · " : "", detail ? detail : "");
                        native_display->SetNesStatus(buf);
                    },
                .set_mode =
                    [native_display](bool playing) { native_display->SetNesPlaying(playing); },
            });
            fc_emulator_service_.SetDirectFrameCallback(
                [native_display](const uint16_t* pixels, uint16_t width, uint16_t height) -> bool {
                    native_display->PushNesFrame(pixels, width, height);
                    return true;
                });
            // Direct-to-panel NES video (LVGL paused while a ROM runs). Falls
            // back to the LVGL image path above if the panel has 1 buffer.
            if (tab5_nes_video::Init(native_display->lcd_panel(), native_display->lv_disp())) {
                {
                    Settings settings("fc", false);
                    tab5_nes_video::SetAspect(
                        settings.GetInt("aspect", tab5_nes_video::kAspectPixelPerfect));
                }
                fc_emulator_service_.SetVideoSessionHooks(
                    [] { tab5_nes_video::Begin(); },
                    [] { tab5_nes_video::End(); });
                fc_emulator_service_.SetIndexedFrameCallback(
                    [](const uint8_t* const* lines, const uint16_t* palette, uint16_t width,
                       uint16_t height) -> bool {
                        if (!tab5_nes_video::Active()) {
                            return false;
                        }
                        // Select+Left: pixel-perfect 3x, Select+Right: 4:3.
                        static uint8_t last_pad = 0;
                        const uint8_t pad = UsbGamepadNesMask();
                        const uint8_t pressed = static_cast<uint8_t>(pad & ~last_pad);
                        last_pad = pad;
                        if (pad & kNesBtnSelect) {
                            int aspect = -1;
                            if (pressed & kNesBtnLeft) aspect = tab5_nes_video::kAspectPixelPerfect;
                            if (pressed & kNesBtnRight) aspect = tab5_nes_video::kAspect4x3;
                            if (aspect >= 0 && aspect != tab5_nes_video::GetAspect()) {
                                tab5_nes_video::SetAspect(aspect);
                                // NVS write off the emulator task (its stack is in PSRAM).
                                Application::GetInstance().Schedule([aspect] {
                                    Settings settings("fc", true);
                                    settings.SetInt("aspect", aspect);
                                });
                            }
                        }
                        return tab5_nes_video::Present(lines, palette, width, height);
                    });
            }
        }
#endif
#if !CONFIG_QDTECH_TAB5_NATIVE_UI
        if (!time_weather_started_ && display_) {
            auto* desktop = static_cast<QdtechTab5Display*>(display_)->GetDesktopUI();
            time_weather_service_.Start(desktop);
            photo_service_.Start(desktop);
            FirmwareUpdateService::GetInstance().Start(desktop);
            desktop->SetRadioActions(
                [this]() { radio_service_.PlayPause(); },
                [this]() { radio_service_.Stop(); },
                [this]() { radio_service_.Next(); },
                [this]() { radio_service_.Prev(); });
            desktop->SetMusicActions(
                [this]() { radio_service_.Play(); },
                [this]() { radio_service_.Pause(); },
                [this]() { radio_service_.Next(); });
            desktop->SetMusicReplayCallback(
                [this](const std::string& title, const std::string& artist,
                       const std::string& url, const std::string&) {
                    radio_service_.PlayUrlFromTool(title, artist, url);
                });
            radio_service_.Start(desktop);
            podcast_service_.Start(desktop);
            desktop->podcast_stop_other_media_ = [this]() { radio_service_.Stop(); };
            fc_emulator_service_.Start({
                .set_state = [desktop](const char* title, const char* detail, const char* list) {
                    desktop->SetFcState(title, detail, list);
                },
                .set_mode = [desktop](bool playing) { desktop->SetFcMode(playing); },
                .set_frame = [desktop](const lv_img_dsc_t* image) { desktop->SetFcFrame(image); },
            });
            desktop->SetFcActiveCallback([this](bool active) { fc_emulator_service_.SetActive(active); });
            desktop->SetFcActions(
                [this]() { fc_emulator_service_.PlayPause(); },
                [this]() { fc_emulator_service_.Stop(); },
                [this]() { fc_emulator_service_.Next(); },
                [this]() { fc_emulator_service_.Prev(); });
            desktop->SetFcControllerCallback(
                [this](uint8_t c) { fc_emulator_service_.SetController(c); });
            desktop->fc_stop_other_media_ = [this]() {
                radio_service_.Stop();
                podcast_service_.Stop();
            };
            RegisterProductTools();
            time_weather_started_ = true;
        }
#endif
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Tab5AudioCodec audio_codec(i2c_bus_, 
                                        AUDIO_INPUT_SAMPLE_RATE, 
                                        AUDIO_OUTPUT_SAMPLE_RATE,
                                        AUDIO_I2S_GPIO_MCLK, 
                                        AUDIO_I2S_GPIO_BCLK, 
                                        AUDIO_I2S_GPIO_WS,
                                        AUDIO_I2S_GPIO_DOUT, 
                                        AUDIO_I2S_GPIO_DIN, 
                                        AUDIO_CODEC_PA_PIN,
                                        AUDIO_CODEC_ES8388_ADDR, 
                                        AUDIO_CODEC_ES7210_ADDR, 
                                        AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }

    void PrepareForNetwork() override {
        // MQTT/TLS need internal SRAM; park camera and stop NES first.
        if (camera_) {
            camera_->PauseStream();
        }
        if (fc_emulator_service_.RomCount() >= 0) {
            fc_emulator_service_.SetActive(false);
            fc_emulator_service_.Stop();
        }
        ESP_LOGI(TAG, "prepare network free_sram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    // BSP power control functions
    void SetChargeQcEn(bool en) {
        if (pi4ioe2_) {
            uint8_t value = pi4ioe2_->ReadOutSet();
            if (en) {
                clrbit(value, 5);  // P5 = CHG_QC_EN (低电平使能)
            } else {
                setbit(value, 5);
            }
            pi4ioe2_->WriteOutSet(value);
        }
    }

    void SetChargeEn(bool en) {
        if (pi4ioe2_) {
            uint8_t value = pi4ioe2_->ReadOutSet();
            if (en) {
                setbit(value, 7);  // P7 = CHG_EN
            } else {
                clrbit(value, 7);
            }
            pi4ioe2_->WriteOutSet(value);
        }
    }

    void SetUsb5vEn(bool en) {
        if (pi4ioe2_) {
            uint8_t value = pi4ioe2_->ReadOutSet();
            if (en) {
                setbit(value, 3);  // P3 = USB5V_EN
            } else {
                clrbit(value, 3);
            }
            pi4ioe2_->WriteOutSet(value);
        }
    }

    void SetExt5vEn(bool en) {
        if (pi4ioe1_) {
            uint8_t value = pi4ioe1_->ReadOutSet();
            if (en) {
                setbit(value, 2);  // P2 = EXT5V_EN
            } else {
                clrbit(value, 2);
            }
            pi4ioe1_->WriteOutSet(value);
        }
    }
};

DECLARE_BOARD(QdtechTab5Board);
