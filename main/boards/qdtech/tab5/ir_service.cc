#include "ir_service.h"

#include <driver/gpio.h>
#include <driver/rmt_rx.h>
#include <driver/rmt_tx.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define TAG "IrService"

namespace {

struct ScanCtx {
    gpio_num_t gpio;
    volatile uint32_t edges;
};

void IRAM_ATTR EdgeIsr(void* arg) {
    auto* ctx = static_cast<ScanCtx*>(arg);
    uint32_t v = ctx->edges;
    ctx->edges = v + 1;
}

struct LearnCtx {
    rmt_symbol_word_t* symbols;
    size_t capacity;
    volatile size_t received;
    volatile bool done;
};

bool IRAM_ATTR OnRxDone(rmt_channel_handle_t, const rmt_rx_done_event_data_t* edata,
                        void* user) {
    auto* ctx = static_cast<LearnCtx*>(user);
    size_t n = edata->num_symbols;
    // Partial RX: append chunks so long AC frames are not truncated.
    size_t room = ctx->capacity - ctx->received;
    if (n > room)
        n = room;
    for (size_t i = 0; i < n; ++i)
        ctx->symbols[ctx->received + i] = edata->received_symbols[i];
    ctx->received += n;
    if (edata->flags.is_last || ctx->received >= ctx->capacity)
        ctx->done = true;
    return false;  // no high-priority wake
}

}  // namespace

std::string IrService::ScanGpios(const int* gpios, size_t n, int seconds) {
    if (seconds <= 0 || seconds > 20)
        seconds = 5;
    std::vector<ScanCtx> ctx(n);
    for (size_t i = 0; i < n; ++i) {
        ctx[i].gpio = static_cast<gpio_num_t>(gpios[i]);
        ctx[i].edges = 0;
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << gpios[i];
        cfg.mode = GPIO_MODE_INPUT;
        cfg.pull_up_en = GPIO_PULLUP_ENABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        cfg.intr_type = GPIO_INTR_ANYEDGE;
        if (gpio_config(&cfg) != ESP_OK)
            continue;
        gpio_install_isr_service(0);
        gpio_isr_handler_add(ctx[i].gpio, EdgeIsr, &ctx[i]);
    }

    ESP_LOGI(TAG, "IR scan %d s — press a remote button now", seconds);
    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));

    std::string json = "{";
    bool first = true;
    for (size_t i = 0; i < n; ++i) {
        gpio_isr_handler_remove(ctx[i].gpio);
        char buf[32];
        snprintf(buf, sizeof(buf), "%s\"%d\":%u", first ? "" : ",", gpios[i],
                 (unsigned)ctx[i].edges);
        json += buf;
        first = false;
        ESP_LOGI(TAG, "  GPIO%-2d edges=%u", gpios[i], (unsigned)ctx[i].edges);
    }
    json += "}";
    return json;
}

std::string IrService::LearnFrame(int rx_gpio, int timeout_ms) {
    if (timeout_ms <= 0 || timeout_ms > 30000)
        timeout_ms = 8000;

    std::lock_guard<std::mutex> guard(rmt_mutex_);

    // A prior gpio_config/ISR scan can leave the pad routed away from RMT.
    gpio_reset_pin(static_cast<gpio_num_t>(rx_gpio));

    // DMA-capable buffer. AC frames can exceed 128 symbols — don't truncate.
    constexpr size_t kCap = 512;
    auto* symbols = static_cast<rmt_symbol_word_t*>(
        heap_caps_malloc(kCap * sizeof(rmt_symbol_word_t), MALLOC_CAP_DMA));
    if (!symbols)
        return "{\"ok\":false,\"error\":\"oom for rx buffer\"}";

    LearnCtx ctx = {};
    ctx.symbols = symbols;
    ctx.capacity = kCap;
    ctx.received = 0;
    ctx.done = false;

    rmt_channel_handle_t rx_chan = nullptr;
    rmt_rx_channel_config_t rx_cfg = {};
    rx_cfg.gpio_num = static_cast<gpio_num_t>(rx_gpio);
    rx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    rx_cfg.resolution_hz = 1 * 1000 * 1000;
    // Hardware RMT RAM is small in non-DMA mode (≤64 symbols). The user
    // buffer can be larger — the driver copies out of the HW block.
    rx_cfg.mem_block_symbols = 64;
    rx_cfg.flags.with_dma = 0;
    esp_err_t err = rmt_new_rx_channel(&rx_cfg, &rx_chan);
    if (err != ESP_OK) {
        heap_caps_free(symbols);
        char buf[80];
        snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"rmt new rx %s\"}",
                 esp_err_to_name(err));
        return buf;
    }

    rmt_rx_event_callbacks_t cbs = {};
    cbs.on_recv_done = OnRxDone;
    rmt_rx_register_event_callbacks(rx_chan, &cbs, &ctx);

    rmt_receive_config_t rcfg = {};
    // P4 filter is APB-clocked; 10000 ns overflows the 8-bit glitch filter
    // (ESP_ERR_INVALID_ARG). Official NEC example uses 1250 ns.
    rcfg.signal_range_min_ns = 1250;
    // Longest NEC space is 9000 us; 12 ms ends the frame.
    rcfg.signal_range_max_ns = 12000000;
    rcfg.flags.en_partial_rx = 1;
    err = rmt_enable(rx_chan);
    if (err == ESP_OK)
        err = rmt_receive(rx_chan, symbols, kCap * sizeof(rmt_symbol_word_t), &rcfg);
    if (err != ESP_OK) {
        rmt_disable(rx_chan);
        rmt_del_channel(rx_chan);
        heap_caps_free(symbols);
        char buf[80];
        snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"rmt receive %s\"}",
                 esp_err_to_name(err));
        return buf;
    }

    ESP_LOGI(TAG, "IR learn on GPIO%d — press the button to capture", rx_gpio);
    int waited = 0;
    while (waited < timeout_ms && !ctx.done) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
    rmt_disable(rx_chan);
    rmt_del_channel(rx_chan);

    if (!ctx.done || ctx.received == 0) {
        heap_caps_free(symbols);
        return "{\"ok\":false,\"error\":\"timeout: no IR frame\"}";
    }

    size_t n = ctx.received;
    // Reject electrical noise: a real IR frame has many marks/spaces ≥100 us.
    size_t useful = 0;
    for (size_t i = 0; i < n; ++i) {
        if (symbols[i].duration0 >= 100 || symbols[i].duration1 >= 100)
            ++useful;
    }
    if (n < 3 || useful < 3) {
        heap_caps_free(symbols);
        return "{\"ok\":false,\"error\":\"noise: not an IR frame\"}";
    }

    std::string json = "{\"ok\":true,\"count\":" + std::to_string(n) + ",\"timings_us\":[";
    for (size_t i = 0; i < n; ++i) {
        if (i)
            json += ",";
        json += std::to_string(symbols[i].duration0);
        json += ",";
        json += std::to_string(symbols[i].duration1);
    }
    json += "]}";
    heap_caps_free(symbols);
    ESP_LOGI(TAG, "IR learned %u symbols from GPIO%d", (unsigned)n, rx_gpio);
    return json;
}

bool IrService::SendNec(int tx_gpio, uint8_t addr, uint8_t cmd) {
    // NEC: 9ms mark, 4.5ms space, addr, ~addr, cmd, ~cmd, stop mark.
    uint16_t t[68];
    int k = 0;
    t[k++] = 9000;
    t[k++] = 4500;
    auto emit_byte = [&](uint8_t b) {
        for (int i = 0; i < 8; ++i) {
            t[k++] = 560;
            t[k++] = (b & 1) ? 1690 : 560;
            b >>= 1;
        }
    };
    emit_byte(addr);
    emit_byte((uint8_t)~addr);
    emit_byte(cmd);
    emit_byte((uint8_t)~cmd);
    t[k++] = 560;
    t[k++] = 0;
    return SendRaw(tx_gpio, t, (size_t)k);
}

bool IrService::SendRaw(int tx_gpio, const uint16_t* marks_spaces_us, size_t count) {
    if (!marks_spaces_us || count == 0)
        return false;

    std::lock_guard<std::mutex> guard(rmt_mutex_);
    gpio_reset_pin(static_cast<gpio_num_t>(tx_gpio));

    rmt_channel_handle_t tx_chan = nullptr;
    rmt_tx_channel_config_t tx_cfg = {};
    tx_cfg.gpio_num = static_cast<gpio_num_t>(tx_gpio);
    tx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    tx_cfg.resolution_hz = 1 * 1000 * 1000;  // 1 us tick
    tx_cfg.mem_block_symbols = 64;
    tx_cfg.trans_queue_depth = 4;
    tx_cfg.flags.with_dma = 0;
    if (rmt_new_tx_channel(&tx_cfg, &tx_chan) != ESP_OK)
        return false;

    std::vector<rmt_symbol_word_t> symbols;
    symbols.reserve((count + 1) / 2);
    for (size_t i = 0; i + 1 < count + 1; i += 2) {
        rmt_symbol_word_t sym = {};
        // duration fields are 15-bit (max 32767 us)
        uint32_t d0 = marks_spaces_us[i];
        if (d0 > 32767) d0 = 32767;
        sym.duration0 = (uint16_t)d0;
        sym.level0 = 1;
        if (i + 1 < count) {
            uint32_t d1 = marks_spaces_us[i + 1];
            if (d1 > 32767) d1 = 32767;
            sym.duration1 = (uint16_t)d1;
            sym.level1 = 0;
        }
        symbols.push_back(sym);
    }

    rmt_copy_encoder_config_t enc_cfg = {};
    rmt_encoder_handle_t encoder = nullptr;
    if (rmt_new_copy_encoder(&enc_cfg, &encoder) != ESP_OK) {
        rmt_del_channel(tx_chan);
        return false;
    }

    // Carrier must be attached before the channel is enabled.
    rmt_carrier_config_t carrier_cfg = {};
    carrier_cfg.duty_cycle = 0.5f;
    carrier_cfg.frequency_hz = 38000;
    if (rmt_apply_carrier(tx_chan, &carrier_cfg) != ESP_OK) {
        rmt_del_encoder(encoder);
        rmt_del_channel(tx_chan);
        return false;
    }

    rmt_enable(tx_chan);
    esp_err_t err = ESP_OK;
    // IR LED drivers are sometimes active-low. Try both polarities so one
    // of them matches the hardware. Reset the copy encoder between bursts.
    const int kLevels[2][2] = {{1, 0}, {0, 1}};  // mark/space levels
    for (int pol = 0; pol < 2 && err == ESP_OK; ++pol) {
        for (size_t i = 0; i < symbols.size(); ++i) {
            symbols[i].level0 = kLevels[pol][0];
            symbols[i].level1 = kLevels[pol][1];
        }
        for (int rep = 0; rep < 2 && err == ESP_OK; ++rep) {
            if (rep || pol)
                vTaskDelay(pdMS_TO_TICKS(50));
            rmt_encoder_reset(encoder);
            rmt_transmit_config_t txcfg = {};
            txcfg.loop_count = 0;
            err = rmt_transmit(tx_chan, encoder, symbols.data(),
                               symbols.size() * sizeof(rmt_symbol_word_t), &txcfg);
            if (err == ESP_OK)
                err = rmt_tx_wait_all_done(tx_chan, 2000);
        }
    }
    rmt_disable(tx_chan);
    rmt_del_encoder(encoder);
    rmt_del_channel(tx_chan);
    return err == ESP_OK;
}

std::string IrService::Loopback(int tx_gpio, int rx_gpio, const uint16_t* marks_spaces_us,
                                size_t count, int timeout_ms) {
    if (!marks_spaces_us || count == 0)
        return "{\"ok\":false,\"error\":\"empty frame\"}";
    if (timeout_ms <= 0 || timeout_ms > 10000)
        timeout_ms = 2000;

    std::lock_guard<std::mutex> guard(rmt_mutex_);
    gpio_reset_pin(static_cast<gpio_num_t>(tx_gpio));
    gpio_reset_pin(static_cast<gpio_num_t>(rx_gpio));

    constexpr size_t kCap = 512;
    auto* symbols = static_cast<rmt_symbol_word_t*>(
        heap_caps_malloc(kCap * sizeof(rmt_symbol_word_t), MALLOC_CAP_DMA));
    if (!symbols)
        return "{\"ok\":false,\"error\":\"oom\"}";
    LearnCtx ctx = {};
    ctx.symbols = symbols;
    ctx.capacity = kCap;

    rmt_channel_handle_t rx_chan = nullptr;
    rmt_rx_channel_config_t rx_cfg = {};
    rx_cfg.gpio_num = static_cast<gpio_num_t>(rx_gpio);
    rx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    rx_cfg.resolution_hz = 1000000;
    rx_cfg.mem_block_symbols = 64;
    rx_cfg.flags.with_dma = 0;
    if (rmt_new_rx_channel(&rx_cfg, &rx_chan) != ESP_OK) {
        heap_caps_free(symbols);
        return "{\"ok\":false,\"error\":\"rx channel\"}";
    }
    rmt_rx_event_callbacks_t cbs = {};
    cbs.on_recv_done = OnRxDone;
    rmt_rx_register_event_callbacks(rx_chan, &cbs, &ctx);
    rmt_receive_config_t rcfg = {};
    rcfg.signal_range_min_ns = 1250;
    rcfg.signal_range_max_ns = 12000000;
    if (rmt_enable(rx_chan) != ESP_OK ||
        rmt_receive(rx_chan, symbols, kCap * sizeof(rmt_symbol_word_t), &rcfg) != ESP_OK) {
        rmt_del_channel(rx_chan);
        heap_caps_free(symbols);
        return "{\"ok\":false,\"error\":\"rx start\"}";
    }

    // TX once with mark=high polarity only (loopback compare).
    rmt_channel_handle_t tx_chan = nullptr;
    rmt_tx_channel_config_t tx_cfg = {};
    tx_cfg.gpio_num = static_cast<gpio_num_t>(tx_gpio);
    tx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    tx_cfg.resolution_hz = 1000000;
    tx_cfg.mem_block_symbols = 64;
    tx_cfg.trans_queue_depth = 1;
    if (rmt_new_tx_channel(&tx_cfg, &tx_chan) != ESP_OK) {
        rmt_disable(rx_chan);
        rmt_del_channel(rx_chan);
        heap_caps_free(symbols);
        return "{\"ok\":false,\"error\":\"tx channel\"}";
    }
    std::vector<rmt_symbol_word_t> tx_sym;
    for (size_t i = 0; i + 1 < count + 1; i += 2) {
        rmt_symbol_word_t sym = {};
        sym.duration0 = marks_spaces_us[i] > 32767 ? 32767 : marks_spaces_us[i];
        sym.level0 = 1;
        if (i + 1 < count) {
            sym.duration1 = marks_spaces_us[i + 1] > 32767 ? 32767 : marks_spaces_us[i + 1];
            sym.level1 = 0;
        }
        tx_sym.push_back(sym);
    }
    rmt_copy_encoder_config_t enc_cfg = {};
    rmt_encoder_handle_t encoder = nullptr;
    rmt_new_copy_encoder(&enc_cfg, &encoder);
    rmt_carrier_config_t carrier_cfg = {};
    carrier_cfg.duty_cycle = 0.5f;
    carrier_cfg.frequency_hz = 38000;
    rmt_apply_carrier(tx_chan, &carrier_cfg);
    rmt_enable(tx_chan);
    rmt_transmit_config_t txcfg = {};
    rmt_transmit(tx_chan, encoder, tx_sym.data(),
                 tx_sym.size() * sizeof(rmt_symbol_word_t), &txcfg);
    rmt_tx_wait_all_done(tx_chan, 2000);

    int waited = 0;
    while (waited < timeout_ms && !ctx.done) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
    rmt_disable(tx_chan);
    rmt_del_encoder(encoder);
    rmt_del_channel(tx_chan);
    rmt_disable(rx_chan);
    rmt_del_channel(rx_chan);

    if (!ctx.done || ctx.received == 0) {
        heap_caps_free(symbols);
        return "{\"ok\":false,\"error\":\"loopback heard nothing\"}";
    }
    size_t n = ctx.received;
    std::string json = "{\"ok\":true,\"count\":" + std::to_string(n) + ",\"timings_us\":[";
    for (size_t i = 0; i < n; ++i) {
        if (i) json += ",";
        json += std::to_string(symbols[i].duration0);
        json += ",";
        json += std::to_string(symbols[i].duration1);
    }
    json += "]}";
    heap_caps_free(symbols);
    return json;
}
