#include "tab5_audio_codec.h"

#include <esp_log.h>
#include <driver/i2c.h>
#include <driver/i2s_tdm.h>

#define TAG "Tab5AudioCodec"

Tab5AudioCodec::Tab5AudioCodec(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
    gpio_num_t pa_pin, uint8_t es8388_addr, uint8_t es7210_addr, bool input_reference) {
    duplex_ = true; // 是否双工
    input_reference_ = input_reference; // 是否使用参考输入，实现回声消除
    input_channels_ = input_reference_ ? 2 : 1; // 输入通道数
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    input_gain_ = 30;

    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    // Do initialize of related interface: data_if, ctrl_if and gpio_if
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    // Output
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = (i2c_port_t)1,
        .addr = es8388_addr,
        .bus_handle = i2c_master_handle,
    };
    out_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(out_ctrl_if_ != NULL);

    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != NULL);
    es8388_codec_cfg_t es8388_cfg = {};
    es8388_cfg.ctrl_if = out_ctrl_if_;
    es8388_cfg.gpio_if = gpio_if_;
    es8388_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8388_cfg.master_mode = true;
    es8388_cfg.pa_pin = -1; // PI4IOE1 P1 控制 
    es8388_cfg.pa_reverted = false;
    es8388_cfg.hw_gain.pa_voltage = 5.0;
    es8388_cfg.hw_gain.codec_dac_voltage = 3.3;
    out_codec_if_ = es8388_codec_new(&es8388_cfg);
    assert(out_codec_if_ != NULL);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = out_codec_if_,
        .data_if = data_if_,
    };
    output_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(output_dev_ != NULL);

    // Input
    i2c_cfg.addr = es7210_addr;
    in_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(in_ctrl_if_ != NULL);

    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = in_ctrl_if_;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    in_codec_if_ = es7210_codec_new(&es7210_cfg);
    assert(in_codec_if_ != NULL);

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = in_codec_if_;
    input_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(input_dev_ != NULL);

    ESP_LOGI(TAG, "Tab5 AudioDevice initialized");
}

Tab5AudioCodec::~Tab5AudioCodec() {
    ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
    esp_codec_dev_delete(output_dev_);
    ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    esp_codec_dev_delete(input_dev_);

    audio_codec_delete_codec_if(in_codec_if_);
    audio_codec_delete_ctrl_if(in_ctrl_if_);
    audio_codec_delete_codec_if(out_codec_if_);
    audio_codec_delete_ctrl_if(out_ctrl_if_);
    audio_codec_delete_gpio_if(gpio_if_);
    audio_codec_delete_data_if(data_if_);
}

void Tab5AudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    assert(input_sample_rate_ == output_sample_rate_);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        // 8x256 ≈ 85 ms at 24 kHz. Larger values steal scarce internal SRAM
        // from TLS/MQTT and caused panics when DMA hit the buffer-size cap
        // (driver silently reduced 512 -> 504 and heap dropped ~20 KB).
        .dma_desc_num = 8,
        .dma_frame_num = 256,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)input_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = i2s_tdm_slot_mask_t(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
            .skip_mask = false,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = I2S_GPIO_UNUSED,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx_handle_, &tdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    ESP_LOGI(TAG, "Duplex channels created");
}

void Tab5AudioCodec::SetOutputVolume(int volume) {
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, volume));
    AudioCodec::SetOutputVolume(volume);
}

void Tab5AudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(codec_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 4,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
            .sample_rate = (uint32_t)output_sample_rate_,
            .mclk_multiple = 0,
        };
        if (input_reference_) {
            fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
        }
        if (esp_codec_dev_open(input_dev_, &fs) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open input device");
            return;
        }
        if (esp_codec_dev_set_in_channel_gain(input_dev_, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), input_gain_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set input gain");
        }
    } else {
        if (esp_codec_dev_close(input_dev_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to close input device");
        }
    }
    AudioCodec::EnableInput(enable);
    // Mic power-off reconfigures shared duplex I2S and can leave TX disabled.
    // Skip the expensive reopen when radio/music already claimed external
    // playback and output is healthy — a mid-stream ReopenOutput is an audible
    // stutter. Only restore when TX might have been parked (voice path).
    if (output_enabled_ && !external_playback_.load(std::memory_order_relaxed)) {
        ESP_LOGI(TAG, "Restoring TX after input %s", enable ? "on" : "off");
        ReopenOutputLocked();
    }
}

void Tab5AudioCodec::ReopenOutputLocked() {
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = (uint32_t)output_sample_rate_,
        .mclk_multiple = 0,
    };
    // Reconfiguring TX while RX is open leaves I2S with "Pending out channel
    // for in channel running" and playback goes silent while output_enabled_
    // stays true. Close mic first, restore TX, then put the mic back.
    const bool restore_input = input_enabled_;
    if (restore_input) {
        esp_codec_dev_close(input_dev_);
        AudioCodec::EnableInput(false);
    }
    esp_codec_dev_close(output_dev_);
    if (esp_codec_dev_open(output_dev_, &fs) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reopen output device");
        AudioCodec::EnableOutput(false);
    } else {
        if (esp_codec_dev_set_out_vol(output_dev_, output_volume_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set output volume");
        }
        AudioCodec::EnableOutput(true);
        ESP_LOGI(TAG, "TX device reopened");
    }
    if (restore_input) {
        esp_codec_dev_sample_info_t in_fs = {
            .bits_per_sample = 16,
            .channel = 4,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
            .sample_rate = (uint32_t)output_sample_rate_,
            .mclk_multiple = 0,
        };
        if (input_reference_) {
            in_fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
        }
        if (esp_codec_dev_open(input_dev_, &in_fs) == ESP_OK) {
            esp_codec_dev_set_in_channel_gain(input_dev_,
                ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), input_gain_);
            AudioCodec::EnableInput(true);
        } else {
            ESP_LOGE(TAG, "Failed to restore input after output reopen");
        }
    }
}

void Tab5AudioCodec::ReopenOutput() {
    std::lock_guard<std::mutex> lock(codec_mutex_);
    if (!output_enabled_) {
        // fall through to open path below without recursion
    } else {
        ReopenOutputLocked();
        return;
    }
    ReopenOutputLocked();
}

void Tab5AudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(codec_mutex_);
    if (enable == output_enabled_) {
        return;
    }
    if (enable) {
        ReopenOutputLocked();
    } else {
        if (esp_codec_dev_close(output_dev_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to close output device");
        }
        AudioCodec::EnableOutput(false);
    }
}

int Tab5AudioCodec::Read(int16_t* dest, int samples) {
    std::lock_guard<std::mutex> lock(codec_mutex_);
    if (input_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(input_dev_, (void*)dest, samples * sizeof(int16_t)));
    }
    return samples;
}

int Tab5AudioCodec::Write(const int16_t* data, int samples) {
    std::lock_guard<std::mutex> lock(codec_mutex_);
    if (output_enabled_) {
        int err = esp_codec_dev_write(output_dev_, (void*)data, samples * sizeof(int16_t));
        if (err != 0) {
            ++write_failures_;
            if (write_failures_ == 1 || (write_failures_ % 32) == 0) {
                ESP_LOGW(TAG, "output write failed err=%d count=%u", err,
                         static_cast<unsigned>(write_failures_));
            }
            if (write_failures_ >= 4) {
                write_failures_ = 0;
                ESP_LOGW(TAG, "output write failing; reopening TX");
                ReopenOutputLocked();
            }
        } else {
            write_failures_ = 0;
        }
    }
    return samples;
}
