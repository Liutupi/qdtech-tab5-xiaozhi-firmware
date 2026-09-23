#include "application.h"
#include "button.h"
#include "display/lcd_display.h"
#include "desktop_ui.h"
#include "esp_cam_sensor_xclk.h"
#include "esp_lcd_ili9881c.h"
#include "esp_lcd_st7121.h"
#include "esp_lcd_st7123.h"

// config.h declares panel initialization tables using the driver types above.
#include "config.h"
#include "esp_video.h"
#include "esp_video_init.h"
#include "tab5_audio_codec.h"
#include "tab5_sd.h"
#include "time_weather_service.h"
#include "photo_service.h"
#include "firmware_update_service.h"
#include "radio_service.h"
#include "podcast_service.h"
#include "fc_emulator_service.h"
#include "mcp_server.h"
#include "wifi_board.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_idf_version.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <cstring>
#include "esp_check.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_touch_st7123.h"
#include "esp_lvgl_port.h"
#include "esp_ldo_regulator.h"
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

class QdtechTab5Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    LcdDisplay* display_;
    EspVideo* camera_ = nullptr;
    Pi4ioe1* pi4ioe1_;
    Pi4ioe2* pi4ioe2_;
    esp_lcd_touch_handle_t touch_ = nullptr;
    TimeWeatherService time_weather_service_;
    PhotoService photo_service_;
    RadioService radio_service_;
    PodcastService podcast_service_;
    FcEmulatorService fc_emulator_service_;
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
                return radio_service_.PlayUrlFromTool(
                    p["title"].value<std::string>(), p["artist"].value<std::string>(),
                    p["url"].value<std::string>());
            });
        mcp.AddTool("self.music.stop", "Stop MP3 URL playback.", PropertyList(),
            [this](const PropertyList&) -> ReturnValue { radio_service_.Stop(); return true; });
    }
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
                    if (board) board->ProcessDesktopTouch(false);
                    return;
                }
                uint8_t count = 0;
                esp_lcd_touch_point_data_t point[1] = {};
                if (esp_lcd_touch_get_data(touch, point, &count, 1) == ESP_OK && count) {
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
                } else {
                    board->ProcessDesktopTouch(false);
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
        dpi_config.num_fbs = 1;
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
    }

public:
    QdtechTab5Board() : boot_button_(BOOT_BUTTON_GPIO) {
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
    }

    void StartNetwork() override {
        WifiBoard::StartNetwork();
        // The display is set up before WifiBoard starts ESP-Hosted. Retry the
        // SD slot after hosted has initialized the P4's shared controller.
        if (!Tab5SdReady()) {
            lvgl_port_lock(0);
            Tab5SdMountAndRegisterFs();
            lvgl_port_unlock();
        }
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
            fc_emulator_service_.Start(desktop);
            desktop->fc_stop_other_media_ = [this]() {
                radio_service_.Stop();
                podcast_service_.Stop();
            };
            RegisterProductTools();
            time_weather_started_ = true;
        }
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
