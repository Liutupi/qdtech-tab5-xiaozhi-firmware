#include "md_board.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7121.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "md_board";

// Same wiring as main/boards/qdtech/tab5/config.h and m5stack_tab5.cc.
#define I2C_SDA GPIO_NUM_31
#define I2C_SCL GPIO_NUM_32
#define BACKLIGHT_GPIO GPIO_NUM_22
#define TOUCH_INT_GPIO GPIO_NUM_23
#define ST712X_TOUCH_ADDR 0x55
#define DSI_PHY_LDO_CHAN 3
#define DSI_PHY_LDO_MV 2500

#define PI4_CHIP_RESET 0x01
#define PI4_IO_DIR 0x03
#define PI4_OUT_SET 0x05
#define PI4_OUT_H_IM 0x07
#define PI4_IN_DEF_STA 0x09
#define PI4_PULL_EN 0x0B
#define PI4_PULL_SEL 0x0D
#define PI4_INT_MASK 0x11

static i2c_master_dev_handle_t add_dev(i2c_master_bus_handle_t bus, uint8_t addr) {
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t dev = NULL;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &cfg, &dev));
    return dev;
}

static void wr(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    ESP_ERROR_CHECK(i2c_master_transmit(dev, buf, 2, 100));
}

static uint8_t rd(i2c_master_dev_handle_t dev, uint8_t reg) {
    uint8_t value = 0;
    ESP_ERROR_CHECK(i2c_master_transmit_receive(dev, &reg, 1, &value, 1, 100));
    return value;
}

// PI4IOE #1 (0x43): P1 SPK_EN, P2 EXT5V_EN, P4 LCD_RST, P5 TP_RST, P6 CAM_RST.
// PI4IOE #2 (0x44): P0 WLAN_PWR_EN, P3 USB5V_EN (gamepad power), P7 CHG_EN.
static void expanders_init(i2c_master_bus_handle_t bus) {
    i2c_master_dev_handle_t e1 = add_dev(bus, 0x43);
    wr(e1, PI4_CHIP_RESET, 0xFF);
    (void)rd(e1, PI4_CHIP_RESET);
    wr(e1, PI4_IO_DIR, 0b01101111);
    wr(e1, PI4_OUT_H_IM, 0b00000000);
    wr(e1, PI4_PULL_SEL, 0b01111111);
    wr(e1, PI4_PULL_EN, 0b01111111);
    wr(e1, PI4_IN_DEF_STA, 0b10000000);
    wr(e1, PI4_INT_MASK, 0b01111111);
    wr(e1, PI4_OUT_SET, 0b01110110);

    i2c_master_dev_handle_t e2 = add_dev(bus, 0x44);
    wr(e2, PI4_CHIP_RESET, 0xFF);
    (void)rd(e2, PI4_CHIP_RESET);
    wr(e2, PI4_IO_DIR, 0b10111001);
    wr(e2, PI4_OUT_H_IM, 0b00000110);
    wr(e2, PI4_PULL_SEL, 0b10111001);
    wr(e2, PI4_PULL_EN, 0b11111001);
    wr(e2, PI4_IN_DEF_STA, 0b01000000);
    wr(e2, PI4_INT_MASK, 0b10111111);
    wr(e2, PI4_OUT_SET, 0b10001001);

    // Pulse LCD and touch reset (ST712x wants LCD_RST released as an input with pull-up).
    gpio_reset_pin(TOUCH_INT_GPIO);
    uint8_t out = rd(e1, PI4_OUT_SET);
    out &= ~(1u << 5);
    out &= ~(1u << 4);
    wr(e1, PI4_OUT_SET, out);
    wr(e1, PI4_IO_DIR, rd(e1, PI4_IO_DIR) | (1u << 4));
    vTaskDelay(pdMS_TO_TICKS(100));
    out |= (1u << 5);
    wr(e1, PI4_OUT_SET, out);
    wr(e1, PI4_PULL_SEL, rd(e1, PI4_PULL_SEL) | (1u << 4));
    wr(e1, PI4_PULL_EN, rd(e1, PI4_PULL_EN) | (1u << 4));
    wr(e1, PI4_IO_DIR, rd(e1, PI4_IO_DIR) & ~(1u << 4));
    vTaskDelay(pdMS_TO_TICKS(100));
}

// ST712x boards report their touch firmware version at register 0: 1 = ST7121, 3 = ST7123.
static int st712x_version(i2c_master_bus_handle_t bus) {
    if (i2c_master_probe(bus, ST712X_TOUCH_ADDR, 200) != ESP_OK)
        return -1;
    esp_lcd_panel_io_i2c_config_t cfg = {
        .dev_addr = ST712X_TOUCH_ADDR,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = 16,
        .scl_speed_hz = 100000,
        .flags = {.disable_control_phase = 1},
    };
    esp_lcd_panel_io_handle_t io = NULL;
    if (esp_lcd_new_panel_io_i2c(bus, &cfg, &io) != ESP_OK)
        return 3;
    uint8_t version = 0;
    esp_err_t err = esp_lcd_panel_io_rx_param(io, 0x0000, &version, 1);
    esp_lcd_panel_io_del(io);
    return err == ESP_OK ? version : 3;
}

static bool st7121_init(md_board_t* board) {
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = {.chan_id = DSI_PHY_LDO_CHAN, .voltage_mv = DSI_PHY_LDO_MV};
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo));

    esp_lcd_dsi_bus_handle_t bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = {.bus_id = 0, .num_data_lanes = 2, .lane_bit_rate_mbps = 965};
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &bus));
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = {.virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8};
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(bus, &dbi_cfg, &io));

    esp_lcd_dpi_panel_config_t dpi = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 70,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 2,
        .video_timing = {
            .h_size = MD_PANEL_W,
            .v_size = MD_PANEL_H,
            .hsync_pulse_width = 2,
            .hsync_back_porch = 40,
            .hsync_front_porch = 40,
            .vsync_pulse_width = 20,
            .vsync_back_porch = 24,
            .vsync_front_porch = 200,
        },
    };
    st7121_vendor_config_t vendor = {0};
    vendor.mipi_config.dsi_bus = bus;
    vendor.mipi_config.dpi_config = &dpi;
    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 24,
        .vendor_config = &vendor,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7121(io, &dev, &board->panel));
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_enable_dma2d(board->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(board->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(board->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(board->panel, true));
    void* fb0 = NULL;
    void* fb1 = NULL;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(board->panel, 2, &fb0, &fb1));
    board->fb[0] = fb0;
    board->fb[1] = fb1;
    return true;
}

void md_board_backlight(int percent) {
    static bool ready = false;
    if (!ready) {
        ledc_timer_config_t timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = LEDC_TIMER_1,
            .freq_hz = 25000,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ESP_ERROR_CHECK(ledc_timer_config(&timer));
        ledc_channel_config_t channel = {
            .gpio_num = BACKLIGHT_GPIO,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_1,
            .timer_sel = LEDC_TIMER_1,
            .duty = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&channel));
        ready = true;
    }
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, (1023 * percent) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

bool md_board_init(md_board_t* board) {
    memset(board, 0, sizeof(*board));
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 1,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = 1},
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &board->i2c));
    expanders_init(board->i2c);
    const int version = st712x_version(board->i2c);
    if (version != 1) {
        ESP_LOGE(TAG, "panel not supported in MD mode yet (ST712x touch version %d)", version);
        return false;
    }
    ESP_LOGI(TAG, "ST7121 panel");
    return st7121_init(board);
}
