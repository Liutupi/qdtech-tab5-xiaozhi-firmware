#include "application.h"
#include "button.h"
#include "display/lcd_display.h"
#if CONFIG_QDTECH_TAB5_NATIVE_UI
#include "icu_calculators.h"
#include "tab5_music_lyrics.h"
#include "tab5_native_display.h"
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
#include "mcp_server.h"
#include "radio_service.h"
#include "tab5_audio_codec.h"
#include "tab5_sd.h"
#include "tab5_ota.h"
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
    bool native_tools_registered_ = false;

    struct MusicLookupRequest {
        QdtechTab5Board* board;
        uint32_t generation;
        std::string title;
        std::string artist;
        std::string url;
        std::string song_id;
    };

    std::string StartMusicNow(const std::string& title, const std::string& artist,
                              const std::string& url, const std::string& lyrics) {
        auto* display = static_cast<QdtechTab5Display*>(display_);
        display->BeginMusicTrack(title.c_str(), artist.c_str());
        display->SetMusicInfo(title.c_str(), artist.c_str(), "正在连接音源…");
        const auto result = radio_service_.PlayUrlFromTool(title, artist, url);
        if (result.rfind("Music URL was NOT started", 0) == 0) {
            display->StopMusicTrack();
            return result;
        }
        if (!lyrics.empty())
            display->SetMusicLyrics(lyrics.c_str(), title.c_str());
        return result;
    }

    static void MusicLookupTask(void* arg) {
        {
            std::unique_ptr<MusicLookupRequest> request(static_cast<MusicLookupRequest*>(arg));
            vTaskDelay(pdMS_TO_TICKS(250));
            auto lyrics =
                tab5_music_lyrics::Lookup(request->title, request->artist, request->song_id);
            auto* board = request->board;
            const auto generation = request->generation;
            Application::GetInstance().Schedule(
                [board, generation, title = std::move(request->title),
                 artist = std::move(request->artist), url = std::move(request->url),
                 lyrics = std::move(lyrics)] {
                    if (board->music_request_generation_.load() != generation)
                        return;
                    board->StartMusicNow(title, artist, url, lyrics);
                });
        }
        vTaskDelete(nullptr);
    }

    void EnsureNativeRadio() {
        std::lock_guard<std::mutex> guard(native_radio_mutex_);
        if (native_radio_ready_.load())
            return;
        auto* native_display = static_cast<QdtechTab5Display*>(display_);
        radio_service_.Start(
            nullptr, [native_display](const char* station, const char* state, const char* meta) {
                native_display->UpdateMusicPlaybackState(station, state);
                native_display->SetRadioState(station, state, meta);
            });
        native_radio_ready_.store(radio_service_.IsStarted());
        if (native_radio_ready_.load()) {
            native_display->SetRadioState(
                radio_service_.GetStationName(radio_service_.GetCurrentIndex()), "Ready",
                "Tap Play");
        }
    }

    void RegisterNativeTools() {
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
                        ++music_request_generation_;
                        EnsureNativeRadio();
                        const auto station = properties["station"].value<std::string>();
                        if (!station.empty())
                            radio_service_.SelectStation(station);
                        radio_service_.Play();
                        static_cast<QdtechTab5Display*>(display_)->ShowRadioPage();
                        return true;
                    });
        mcp.AddTool("self.radio.stop", "Stop internet radio playback.", PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
                        ++music_request_generation_;
                        if (native_radio_ready_.load())
                            radio_service_.Stop();
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
                        radio_service_.Stop();
                        fc_emulator_service_.SetActive(true);
                        fc_emulator_service_.PlayPause();
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
                        ++music_request_generation_;
                        EnsureNativeRadio();
                        radio_service_.Next();
                        static_cast<QdtechTab5Display*>(display_)->ShowRadioPage();
                        return true;
                    });
        mcp.AddTool("self.radio.previous", "Play the previous internet radio station.",
                    PropertyList(), [this](const PropertyList&) -> ReturnValue {
                        ++music_request_generation_;
                        EnsureNativeRadio();
                        radio_service_.Prev();
                        static_cast<QdtechTab5Display*>(display_)->ShowRadioPage();
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
                if (!display_)
                    return std::string("Display not ready");
                static_cast<QdtechTab5Display*>(display_)->SetDailyCards(titles, bodies);
                return std::string("Daily cards updated");
            });
        mcp.AddTool(
            "self.ir.scan",
            "Probe IR receiver wiring: count signal edges on candidate GPIOs for N seconds. "
            "Press any remote button while it runs. The GPIO with the highest count is the "
            "IR receiver data pin.",
            PropertyList({Property("seconds", kPropertyTypeInteger, 5, 1, 20)}),
            [this](const PropertyList& p) -> ReturnValue {
                static const int kGpios[] = {14, 15, 16, 17, 18, 19, 20, 21,
                                             24, 25, 33, 34, 35, 45, 46, 47,
                                             48, 49, 50, 51, 52, 53, 54};
                const int sec = p["seconds"].value<int>();
                return IrService::GetInstance().ScanGpios(kGpios,
                                                           sizeof(kGpios) / sizeof(kGpios[0]),
                                                           sec);
            });
        mcp.AddTool(
            "self.ir.send_raw",
            "Send a raw IR frame on tx_gpio. Provide mark/space durations in microseconds, "
            "starting with a mark (e.g. NEC: 9000,4500, 560,560, ...).",
            PropertyList({Property("gpio", kPropertyTypeInteger, 53, 0, 54),
                          Property("timings_us", kPropertyTypeString)}),
            [this](const PropertyList& p) -> ReturnValue {
                const int gpio = p["gpio"].value<int>();
                const auto timings = p["timings_us"].value<std::string>();
                std::vector<uint16_t> us;
                size_t pos = 0;
                while (pos < timings.size() && us.size() < 240) {
                    auto comma = timings.find(',', pos);
                    auto token = timings.substr(pos, comma - pos);
                    if (!token.empty())
                        us.push_back((uint16_t)atoi(token.c_str()));
                    if (comma == std::string::npos)
                        break;
                    pos = comma + 1;
                }
                if (us.empty())
                    return std::string("no timings");
                bool ok = IrService::GetInstance().SendRaw(gpio, us.data(), us.size());
                return ok ? std::string("IR sent") : std::string("IR send failed");
            });
        mcp.AddTool(
            "self.ir.learn",
            "Learn one IR frame from an existing remote. Point the remote at the IR receiver "
            "and press a button. Returns timings_us JSON; save it for later self.ir.send_raw. "
            "Default gpio 54 is the Tab5 Port A IR receiver pin.",
            PropertyList({Property("gpio", kPropertyTypeInteger, 54, 0, 54),
                          Property("timeout_ms", kPropertyTypeInteger, 8000, 1000, 30000)}),
            [this](const PropertyList& p) -> ReturnValue {
                return IrService::GetInstance().LearnFrame(p["gpio"].value<int>(),
                                                           p["timeout_ms"].value<int>());
            });
        mcp.AddTool(
            "self.ir.send_nec",
            "Send a NEC IR frame (8-bit addr + cmd). Common for TVs (Xiaomi often uses NEC).",
            PropertyList({Property("gpio", kPropertyTypeInteger, 53, 0, 54),
                          Property("addr", kPropertyTypeInteger, 0, 0, 255),
                          Property("cmd", kPropertyTypeInteger, 0, 0, 255)}),
            [this](const PropertyList& p) -> ReturnValue {
                bool ok = IrService::GetInstance().SendNec(p["gpio"].value<int>(),
                                                           (uint8_t)p["addr"].value<int>(),
                                                           (uint8_t)p["cmd"].value<int>());
                return ok ? std::string("NEC sent") : std::string("NEC send failed");
            });
        mcp.AddTool(
            "self.ir.dump_slot",
            "Dump a learned IR slot from NVS as timings_us JSON.",
            PropertyList({Property("slot", kPropertyTypeInteger, 0, 0, 19)}),
            [](const PropertyList& p) -> ReturnValue {
                const int slot = p["slot"].value<int>();
                char key[16];
                snprintf(key, sizeof(key), "s%d", slot);
                nvs_handle_t h;
                if (nvs_open("ir_slots", NVS_READONLY, &h) != ESP_OK)
                    return std::string("{\"ok\":false,\"error\":\"nvs open\"}");
                size_t len = 0;
                nvs_get_blob(h, key, nullptr, &len);
                std::vector<uint16_t> us(len / 2);
                esp_err_t err = len ? nvs_get_blob(h, key, us.data(), &len) : ESP_ERR_NVS_NOT_FOUND;
                nvs_close(h);
                if (err != ESP_OK || us.empty())
                    return std::string("{\"ok\":false,\"error\":\"slot empty\"}");
                std::string json = "{\"ok\":true,\"count\":" + std::to_string(us.size() / 2) +
                                   ",\"timings_us\":[";
                for (size_t i = 0; i < us.size(); ++i) {
                    if (i) json += ",";
                    json += std::to_string(us[i]);
                }
                json += "]}";
                return json;
            });
        mcp.AddTool(
            "self.ir.loopback",
            "TX a learned slot on gpio 53 while listening on gpio 54. If this hears the frame, "
            "the IR LED path works and the AC unit should see the code.",
            PropertyList({Property("slot", kPropertyTypeInteger, 0, 0, 19)}),
            [](const PropertyList& p) -> ReturnValue {
                const int slot = p["slot"].value<int>();
                char key[16];
                snprintf(key, sizeof(key), "s%d", slot);
                nvs_handle_t h;
                if (nvs_open("ir_slots", NVS_READONLY, &h) != ESP_OK)
                    return std::string("{\"ok\":false,\"error\":\"nvs open\"}");
                size_t len = 0;
                nvs_get_blob(h, key, nullptr, &len);
                std::vector<uint16_t> us(len / 2);
                esp_err_t err = len ? nvs_get_blob(h, key, us.data(), &len) : ESP_ERR_NVS_NOT_FOUND;
                nvs_close(h);
                if (err != ESP_OK || us.empty())
                    return std::string("{\"ok\":false,\"error\":\"slot empty\"}");
                return IrService::GetInstance().Loopback(53, 54, us.data(), us.size(), 2500);
            });
        mcp.AddTool(
            "self.music.play_url",
            "Play a direct HTTP(S) MP3 song URL on Tab5. Pass the exact title and artist from "
            "the NAS/NetEase MCP, and its numeric song_id when available. Pass real full timed "
            "LRC in lyrics if the NAS provides it. Otherwise Tab5 looks up matching NetEase "
            "lyrics before starting audio. Do not invent or read lyrics aloud.",
            PropertyList({Property("title", kPropertyTypeString, std::string("Music")),
                          Property("artist", kPropertyTypeString, std::string("")),
                          Property("url", kPropertyTypeString),
                          Property("lyrics", kPropertyTypeString, std::string("")),
                          Property("song_id", kPropertyTypeString, std::string(""))}),
            [this](const PropertyList& p) -> ReturnValue {
                EnsureNativeRadio();
                const auto title_value = p["title"].value<std::string>();
                const auto title = title_value.empty() ? std::string("Music URL") : title_value;
                const auto artist = p["artist"].value<std::string>();
                const auto url = p["url"].value<std::string>();
                const auto lyrics = p["lyrics"].value<std::string>();
                const auto song_id = p["song_id"].value<std::string>();
                if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
                    return std::string("Music URL was NOT started: invalid HTTP(S) URL.");
                const uint32_t generation = ++music_request_generation_;
                tab5_lrc::Document supplied_lyrics;
                if (tab5_lrc::Parse(lyrics, supplied_lyrics) && supplied_lyrics.timed)
                    return StartMusicNow(title, artist, url, lyrics);
                radio_service_.Stop();
                auto* display = static_cast<QdtechTab5Display*>(display_);
                display->BeginMusicTrack(title.c_str(), artist.c_str());
                display->SetMusicInfo(title.c_str(), artist.c_str(), "正在查找歌词…");
                auto* request = new (std::nothrow)
                    MusicLookupRequest{this, generation, title, artist, url, song_id};
                TaskHandle_t task = nullptr;
                const BaseType_t created =
                    request ? xTaskCreatePinnedToCoreWithCaps(MusicLookupTask, "music_lyrics", 8192,
                                                              request, 2, &task, 0,
                                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                            : pdFAIL;
                if (created != pdPASS) {
                    delete request;
                    return StartMusicNow(title, artist, url, "");
                }
                return std::string(
                    "Song queued on Tab5; matching lyrics are being fetched before audio starts. "
                    "No spoken follow-up is needed.");
            });
        mcp.AddTool(
            "self.music.set_lyrics",
            "Display complete timed LRC lyrics for the currently playing Tab5 song. Retrieve real "
            "LRC from the connected NAS/NetEase MCP first, then pass its raw [mm:ss.xx] lines as "
            "lyrics. Call after self.music.play_url when lyrics were not included there. Do not "
            "fabricate lyrics.",
            PropertyList({Property("title", kPropertyTypeString, std::string("")),
                          Property("lyrics", kPropertyTypeString)}),
            [this](const PropertyList& p) -> ReturnValue {
                const auto title = p["title"].value<std::string>();
                const auto lyrics = p["lyrics"].value<std::string>();
                return static_cast<QdtechTab5Display*>(display_)->SetMusicLyrics(lyrics.c_str(),
                                                                                 title.c_str())
                           ? "Lyrics shown and synchronized on Tab5."
                           : "Lyrics not shown: supply valid LRC for the current song (max 16 KB).";
            });
        mcp.AddTool("self.music.set_lyric",
                    "Show the current lyric line on the Tab5 screen after music starts.",
                    PropertyList({Property("title", kPropertyTypeString, std::string("")),
                                  Property("artist", kPropertyTypeString, std::string("")),
                                  Property("line", kPropertyTypeString, std::string(""))}),
                    [this](const PropertyList& p) -> ReturnValue {
                        static_cast<QdtechTab5Display*>(display_)->SetMusicInfo(
                            p["title"].value<std::string>().c_str(),
                            p["artist"].value<std::string>().c_str(),
                            p["line"].value<std::string>().c_str());
                        return true;
                    });
        mcp.AddTool("self.music.stop", "Stop song playback.", PropertyList(),
                    [this](const PropertyList&) -> ReturnValue {
                        ++music_request_generation_;
                        if (native_radio_ready_.load())
                            radio_service_.Stop();
                        static_cast<QdtechTab5Display*>(display_)->StopMusicTrack();
                        static_cast<QdtechTab5Display*>(display_)->SetMusicInfo("已停止", "",
                                                                                "点歌或收听电台");
                        return true;
                    });
        auto show_icu = [this](int mode, const icu::Result& result) -> ReturnValue {
            static_cast<QdtechTab5Display*>(display_)->ShowIcuPage(mode, result.text);
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
                if (display_) {
                    static_cast<QdtechTab5Display*>(display_)->SetMusicInfo(
                        title.c_str(), artist.c_str(), "正在连接音源…");
                }
                return radio_service_.PlayUrlFromTool(title, artist,
                    p["url"].value<std::string>());
            });
        mcp.AddTool("self.music.set_lyric",
            "Show the current lyric line on the Tab5 screen. Call after self.music.play_url.",
            PropertyList({Property("title", kPropertyTypeString, std::string("")),
                          Property("artist", kPropertyTypeString, std::string("")),
                          Property("line", kPropertyTypeString, std::string(""))}),
            [this](const PropertyList& p) -> ReturnValue {
                if (display_) {
                    static_cast<QdtechTab5Display*>(display_)->SetMusicInfo(
                        p["title"].value<std::string>().c_str(),
                        p["artist"].value<std::string>().c_str(),
                        p["line"].value<std::string>().c_str());
                }
                return true;
            });
        mcp.AddTool("self.music.stop", "Stop MP3 URL playback.", PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                if (display_) {
                    static_cast<QdtechTab5Display*>(display_)->SetMusicInfo(
                        "已停止", "", "点歌或收听电台");
                }
                return radio_service_.Stop();
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

    void I2cDetect() {
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

        camera_ = new EspVideo(video_config);
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
        I2cDetect();
        InitializePi4ioe();
        InitializeDisplay();  // Auto-detect and initialize display + touch
        InitializeCamera();
        InitializeButtons();
        SetChargeQcEn(true);
        SetChargeEn(true);
        SetUsb5vEn(true);
        SetExt5vEn(true);
        GetBacklight()->RestoreBrightness();
        // USB gamepad starts after Wi-Fi/MQTT: starting it at boot leaves too
        // little internal SRAM for `mqtt_client` task creation.
        // UsbGamepadHostStart() is invoked from StartNetwork().
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
                auto* self = static_cast<QdtechTab5Board*>(arg);
                // Let ESP-Hosted finish claiming the shared SDMMC host first.
                vTaskDelay(pdMS_TO_TICKS(800));
                if (!Tab5SdReady()) {
                    Tab5SdMountAndRegisterFs();
                }
                if (self->display_ && Tab5SdReady()) {
                    // lv_binfont_create touches LVGL heap — hold the port lock.
                    lvgl_port_lock(0);
                    static_cast<QdtechTab5Display*>(self->display_)->LoadSdFallbackFont();
                    lvgl_port_unlock();
                }
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
                ++music_request_generation_;
                if (native_radio_ready_.load())
                    radio_service_.Stop();
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
            actions.radio_play_pause = [this] {
                ++music_request_generation_;
                EnsureNativeRadio();
                radio_service_.PlayPause();
            };
            actions.radio_stop = [this] {
                ++music_request_generation_;
                if (native_radio_ready_.load())
                    radio_service_.Stop();
            };
            actions.radio_next = [this] {
                ++music_request_generation_;
                EnsureNativeRadio();
                radio_service_.Next();
            };
            actions.radio_previous = [this] {
                ++music_request_generation_;
                EnsureNativeRadio();
                radio_service_.Prev();
            };
            actions.radio_select = [this](int index) {
                ++music_request_generation_;
                EnsureNativeRadio();
                radio_service_.SelectStationIndex(index);
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
            actions.start_nes = [this] {
                ++music_request_generation_;
                radio_service_.Stop();
                // Game mode: free camera + claim audio so vision/MQTT stay off.
                if (camera_) camera_->PauseStream();
                Application::GetInstance().SetExternalAudioActive(true);
                UsbGamepadSetOnConnect([] {
                    if (g_tab5_camera) g_tab5_camera->PauseStream();
                });
                // USB host needs contiguous internal SRAM. Start it before the
                // FC task claims another block; retry is idempotent.
                UsbGamepadHostStart();
                fc_emulator_service_.SetActive(true);
                fc_emulator_service_.PlayPause();
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
            native_display->SetAppsActions(std::move(actions), [this] {
                EnsureNativeRadio();
                radio_service_.Play();
            });
            Tab5Ota::GetInstance().SetStatusCallback([native_display](const Tab5Ota::Status& status) {
                native_display->SetFirmwareStatus(status.text, status.button, status.progress,
                                                  status.busy);
            });
            RegisterNativeTools();
            native_display->SetPresenceTestAction([this] { vision_service_.RequestPresenceTest(); });
            native_display->SetInteractionAction([this] { vision_service_.NotifyInteraction(); });
            if (camera_) vision_service_.Start(camera_, native_display);
            // NES + USB gamepad (SN30 Pro). Frames can later bind to a native page.
            fc_emulator_service_.Start({
                .set_state = [native_display](const char* title, const char* detail, const char*) {
                    char buf[160];
                    snprintf(buf, sizeof(buf), "%s%s%s", title ? title : "",
                             (detail && *detail) ? " · " : "", detail ? detail : "");
                    native_display->SetNesStatus(buf);
                },
                .set_mode = [native_display](bool playing) {
                    native_display->SetNesPlaying(playing);
                },
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
