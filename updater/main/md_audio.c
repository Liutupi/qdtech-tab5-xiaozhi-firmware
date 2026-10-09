#include "md_audio.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "md_audio_mix.h"
#include "nvs.h"
#include "nvs_flash.h"

#define RATE 24000
#define MAX_SAMPLES (RATE / 50)
#define QUEUE_LEN 4
static const char* TAG = "md_audio";
static esp_codec_dev_handle_t s_codec;
static QueueHandle_t s_queue;
static StaticQueue_t s_queue_control;
typedef struct {
    int count;
    int16_t samples[MAX_SAMPLES * 2];
} audio_frame_t;

static void audio_task(void* arg) {
    audio_frame_t frame;
    uint32_t frames = 0, nonzero = 0;
    while (true) {
        if (xQueueReceive(s_queue, &frame, portMAX_DELAY) != pdTRUE)
            continue;
        for (int i = 0; i < frame.count * 2; ++i)
            if (frame.samples[i]) {
                ++nonzero;
                break;
            }
        const int err = esp_codec_dev_write(s_codec, frame.samples, frame.count * 4);
        if (err != ESP_CODEC_DEV_OK)
            ESP_LOGW(TAG, "DAC write failed %d", err);
        if (++frames % 300 == 0)
            ESP_LOGI(TAG, "DAC frames=%lu nonzero=%lu rate=%d", (unsigned long)frames,
                     (unsigned long)nonzero, RATE);
    }
}

bool md_audio_init(md_board_t* board) {
    i2s_chan_handle_t tx = NULL;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = 6;
    channel.dma_frame_num = 256;
    channel.auto_clear_after_cb = true;
    if (i2s_new_channel(&channel, &tx, NULL) != ESP_OK)
        return false;
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = GPIO_NUM_30,
                     .bclk = GPIO_NUM_27,
                     .ws = GPIO_NUM_29,
                     .dout = GPIO_NUM_26,
                     .din = I2S_GPIO_UNUSED},
    };
    if (i2s_channel_init_std_mode(tx, &cfg) != ESP_OK || i2s_channel_enable(tx) != ESP_OK)
        return false;
    audio_codec_i2s_cfg_t data_cfg = {.port = I2S_NUM_0, .tx_handle = tx};
    const audio_codec_data_if_t* data = audio_codec_new_i2s_data(&data_cfg);
    audio_codec_i2c_cfg_t ctrl_cfg = {
        .port = 1, .addr = ES8388_CODEC_DEFAULT_ADDR, .bus_handle = board->i2c};
    const audio_codec_ctrl_if_t* ctrl = audio_codec_new_i2c_ctrl(&ctrl_cfg);
    const audio_codec_gpio_if_t* gpio = audio_codec_new_gpio();
    if (!data || !ctrl || !gpio)
        return false;
    es8388_codec_cfg_t dac_cfg = {.ctrl_if = ctrl,
                                  .gpio_if = gpio,
                                  .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
                                  .master_mode = true,
                                  .pa_pin = -1,
                                  .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3}};
    const audio_codec_if_t* dac = es8388_codec_new(&dac_cfg);
    if (!dac)
        return false;
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = dac, .data_if = data};
    s_codec = esp_codec_dev_new(&dev_cfg);
    esp_codec_dev_sample_info_t info = {.bits_per_sample = 16, .channel = 2, .sample_rate = RATE};
    if (!s_codec || esp_codec_dev_open(s_codec, &info) != ESP_CODEC_DEV_OK)
        return false;
    nvs_flash_init();  // preserve NVS; do not erase if initialization fails
    int32_t volume = 40;
    nvs_handle_t audio;
    if (nvs_open("audio", NVS_READONLY, &audio) == ESP_OK) {
        nvs_get_i32(audio, "output_volume", &volume);
        nvs_close(audio);
    }
    if (volume < 0)
        volume = 0;
    if (volume > 100)
        volume = 100;
    esp_codec_dev_set_out_vol(s_codec, volume);
    esp_codec_dev_set_out_mute(s_codec, false);
    uint8_t* storage =
        heap_caps_malloc(QUEUE_LEN * sizeof(audio_frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!storage)
        return false;
    s_queue = xQueueCreateStatic(QUEUE_LEN, sizeof(audio_frame_t), storage, &s_queue_control);
    if (!s_queue || xTaskCreate(audio_task, "md_audio", 4096, NULL, 5, NULL) != pdPASS)
        return false;
    ESP_LOGI(TAG, "ES8388 ready at %d Hz, volume=%ld", RATE, (long)volume);
    return true;
}

void md_audio_submit(const int16_t* fm, size_t fm_count, const int16_t* psg, size_t psg_count,
                     int fps) {
    if (!s_queue)
        return;
    audio_frame_t frame;
    frame.count = RATE / (fps == 50 ? 50 : 60);
    md_mix_audio(fm, fm_count, psg, psg_count, frame.samples, frame.count);
    // Bounded: neither the emulator nor exit controls wait on a slow DAC.
    if (xQueueSend(s_queue, &frame, 0) != pdTRUE) {
        audio_frame_t old;
        xQueueReceive(s_queue, &old, 0);
        xQueueSend(s_queue, &frame, 0);
    }
}
void md_audio_mute(void) {
    if (s_codec)
        esp_codec_dev_set_out_mute(s_codec, true);
}
