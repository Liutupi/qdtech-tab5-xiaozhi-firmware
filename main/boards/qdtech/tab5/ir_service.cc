#include "ir_service.h"

#include <driver/gpio.h>
#include <driver/rmt_rx.h>
#include <driver/rmt_tx.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#define TAG "IrService"

namespace {

// RMT idle threshold: a space longer than this ends a frame. 30 ms keeps the inter-part gaps of
// most AC protocols (Gree/Chigo ~20 ms) inside one frame; longer gaps are measured separately.
constexpr uint32_t kFrameEndUs = 30000;
constexpr size_t kRxSymbols = 512;       // per frame
constexpr size_t kMaxTimings = 1400;     // whole learned code
constexpr int kFollowUpMs = 300;         // wait this long for another part after a frame
constexpr int kCaptureWindowMs = 1500;   // never capture longer than this after the first edge

struct ScanCtx {
    gpio_num_t gpio;
    volatile uint32_t edges;
};

void IRAM_ATTR EdgeIsr(void* arg) {
    auto* ctx = static_cast<ScanCtx*>(arg);
    uint32_t v = ctx->edges;
    ctx->edges = v + 1;
}

struct RxCtx {
    rmt_symbol_word_t* symbols;
    size_t capacity;
    volatile size_t received;
    volatile bool done;
    volatile int64_t done_us;
};

bool IRAM_ATTR OnRxDone(rmt_channel_handle_t, const rmt_rx_done_event_data_t* edata, void* user) {
    auto* ctx = static_cast<RxCtx*>(user);
    size_t n = edata->num_symbols;
    const size_t room = ctx->capacity - ctx->received;
    if (n > room)
        n = room;
    for (size_t i = 0; i < n; ++i)
        ctx->symbols[ctx->received + i] = edata->received_symbols[i];
    ctx->received += n;
    if (edata->flags.is_last || ctx->received >= ctx->capacity) {
        ctx->done_us = esp_timer_get_time();
        ctx->done = true;
    }
    return false;
}

// One RX channel that can be re-armed for consecutive frames.
class RxSession {
public:
    explicit RxSession(int gpio) {
        gpio_reset_pin(static_cast<gpio_num_t>(gpio));
        symbols_ = static_cast<rmt_symbol_word_t*>(
            heap_caps_malloc(kRxSymbols * sizeof(rmt_symbol_word_t), MALLOC_CAP_DMA));
        if (!symbols_)
            return;
        rmt_rx_channel_config_t cfg = {};
        cfg.gpio_num = static_cast<gpio_num_t>(gpio);
        cfg.clk_src = RMT_CLK_SRC_DEFAULT;
        cfg.resolution_hz = 1000000;
        cfg.mem_block_symbols = 64;
        if (rmt_new_rx_channel(&cfg, &chan_) != ESP_OK) {
            chan_ = nullptr;
            return;
        }
        rmt_rx_event_callbacks_t cbs = {};
        cbs.on_recv_done = OnRxDone;
        rmt_rx_register_event_callbacks(chan_, &cbs, &ctx_);
        ok_ = rmt_enable(chan_) == ESP_OK;
    }
    ~RxSession() {
        if (chan_) {
            rmt_disable(chan_);
            rmt_del_channel(chan_);
        }
        if (symbols_)
            heap_caps_free(symbols_);
    }
    bool ok() const { return ok_ && symbols_; }

    bool Arm() {
        ctx_.symbols = symbols_;
        ctx_.capacity = kRxSymbols;
        ctx_.received = 0;
        ctx_.done = false;
        rmt_receive_config_t rcfg = {};
        // P4 glitch filter is 8-bit APB ticks; 1250 ns is the documented safe value.
        rcfg.signal_range_min_ns = 1250;
        rcfg.signal_range_max_ns = kFrameEndUs * 1000;
        rcfg.flags.en_partial_rx = 1;
        return rmt_receive(chan_, symbols_, kRxSymbols * sizeof(rmt_symbol_word_t), &rcfg) == ESP_OK;
    }

    // Wait for a frame; returns false on timeout or cancellation.
    bool Wait(int timeout_ms, const std::atomic<bool>* cancel = nullptr) {
        for (int waited = 0; waited < timeout_ms; waited += 10) {
            if (ctx_.done)
                return true;
            if (cancel && cancel->load())
                return false;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return ctx_.done;
    }

    // Flatten the captured symbols into [mark, space, ...] (the receiver output is active low,
    // the RMT reports level 0 = IR present).
    std::vector<uint16_t> Timings(uint32_t* duration_us) const {
        std::vector<uint16_t> out;
        uint32_t total = 0;
        for (size_t i = 0; i < ctx_.received; ++i) {
            const auto& s = symbols_[i];
            for (int half = 0; half < 2; ++half) {
                const uint32_t d = half ? s.duration1 : s.duration0;
                if (d == 0)
                    continue;
                total += d;
                // Merge consecutive same-kind durations (keeps strict mark/space alternation).
                const bool mark = (half ? s.level1 : s.level0) == 0;
                const bool expect_mark = out.size() % 2 == 0;
                if (out.empty() && !mark)
                    continue;  // leading idle
                if (mark != expect_mark && !out.empty()) {
                    out.back() = static_cast<uint16_t>(std::min<uint32_t>(65535, out.back() + d));
                    continue;
                }
                out.push_back(static_cast<uint16_t>(std::min<uint32_t>(65535, d)));
            }
        }
        if (duration_us)
            *duration_us = total;
        return out;
    }

    int64_t done_us() const { return ctx_.done_us; }

private:
    rmt_channel_handle_t chan_ = nullptr;
    rmt_symbol_word_t* symbols_ = nullptr;
    RxCtx ctx_ = {};
    bool ok_ = false;
};

bool IsNecRepeat(const std::vector<uint16_t>& f) {
    return f.size() <= 4 && f.size() >= 2 && f[0] > 7000 && f[0] < 11000 && f[1] > 1800 && f[1] < 2800;
}

bool SameFrame(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const int d = std::abs(int(a[i]) - int(b[i]));
        if (d > 200 && d * 4 > int(std::max(a[i], b[i])))
            return false;
    }
    return true;
}

}  // namespace

bool IrService::Learn(int rx_gpio, int timeout_ms, std::vector<uint16_t>* timings, std::string* error,
                      const std::atomic<bool>* cancel) {
    auto fail = [error](const char* why) {
        if (error)
            *error = why;
        return false;
    };
    if (!timings)
        return fail("内部错误");
    timings->clear();
    if (timeout_ms <= 0 || timeout_ms > 30000)
        timeout_ms = 10000;

    std::lock_guard<std::mutex> guard(rmt_mutex_);
    RxSession rx(rx_gpio);
    if (!rx.ok() || !rx.Arm())
        return fail("红外接收初始化失败");

    // First frame: ignore electrical noise (too few edges) and keep waiting.
    std::vector<uint16_t> first;
    int64_t deadline = esp_timer_get_time() + int64_t(timeout_ms) * 1000;
    while (true) {
        const int left = int((deadline - esp_timer_get_time()) / 1000);
        if (left <= 0 || !rx.Wait(left, cancel))
            return fail(cancel && cancel->load() ? "已取消" : "没有收到红外信号，请把遥控器对准接收头再试");
        uint32_t dur = 0;
        first = rx.Timings(&dur);
        if (first.size() >= 8 && !IsNecRepeat(first))
            break;
        rx.Arm();
    }

    std::vector<std::vector<uint16_t>> frames = {first};
    int64_t last_done = rx.done_us();
    const int64_t window_end = last_done + int64_t(kCaptureWindowMs) * 1000;
    std::vector<uint16_t> gaps;
    while (esp_timer_get_time() < window_end && rx.Arm() && rx.Wait(kFollowUpMs)) {
        uint32_t dur = 0;
        auto frame = rx.Timings(&dur);
        const int64_t gap = (rx.done_us() - last_done) - int64_t(dur);
        last_done = rx.done_us();
        if (frame.size() < 4 || IsNecRepeat(frame))
            continue;
        bool duplicate = false;
        for (const auto& f : frames)
            duplicate |= SameFrame(f, frame);
        if (duplicate)
            continue;
        frames.push_back(std::move(frame));
        gaps.push_back(static_cast<uint16_t>(std::clamp<int64_t>(gap, kFrameEndUs, 65535)));
    }

    for (size_t i = 0; i < frames.size(); ++i) {
        auto& f = frames[i];
        if (f.size() % 2 == 0)
            f.pop_back();  // drop the trailing idle space
        timings->insert(timings->end(), f.begin(), f.end());
        if (i + 1 < frames.size())
            timings->push_back(gaps[i]);
        if (timings->size() > kMaxTimings) {
            timings->resize(kMaxTimings | 1);
            break;
        }
    }
    ESP_LOGI(TAG, "learned %u timings in %u frame(s) on GPIO%d", unsigned(timings->size()),
             unsigned(frames.size()), rx_gpio);
    return true;
}

bool IrService::Send(int tx_gpio, const uint16_t* timings, size_t count, bool active_high,
                     uint32_t carrier_hz, int repeat) {
    if (!timings || count == 0)
        return false;
    if (carrier_hz < 20000 || carrier_hz > 60000)
        carrier_hz = 38000;
    repeat = std::clamp(repeat, 1, 5);

    std::lock_guard<std::mutex> guard(rmt_mutex_);
    gpio_reset_pin(static_cast<gpio_num_t>(tx_gpio));

    // Expand into RMT symbols; split durations above the 15-bit field limit.
    const uint32_t mark_level = active_high ? 1 : 0;
    std::vector<rmt_symbol_word_t> symbols;
    symbols.reserve(count + 8);
    std::vector<std::pair<uint32_t, uint32_t>> halves;  // (level, duration)
    for (size_t i = 0; i < count; ++i) {
        uint32_t d = timings[i];
        const uint32_t level = (i % 2 == 0) ? mark_level : !mark_level;
        while (d > 0) {
            const uint32_t part = std::min<uint32_t>(d, 32767);
            halves.push_back({level, part});
            d -= part;
        }
    }
    if (halves.size() % 2)
        halves.push_back({!mark_level, 100});  // end on a short space
    for (size_t i = 0; i < halves.size(); i += 2) {
        rmt_symbol_word_t s = {};
        s.level0 = halves[i].first;
        s.duration0 = halves[i].second;
        s.level1 = halves[i + 1].first;
        s.duration1 = halves[i + 1].second;
        symbols.push_back(s);
    }

    rmt_channel_handle_t tx = nullptr;
    rmt_tx_channel_config_t cfg = {};
    cfg.gpio_num = static_cast<gpio_num_t>(tx_gpio);
    cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    cfg.resolution_hz = 1000000;
    cfg.mem_block_symbols = 64;
    cfg.trans_queue_depth = 2;
    cfg.flags.invert_out = 0;
    if (rmt_new_tx_channel(&cfg, &tx) != ESP_OK)
        return false;
    rmt_carrier_config_t carrier = {};
    carrier.duty_cycle = 0.33f;
    carrier.frequency_hz = carrier_hz;
    // The carrier modulates the "mark" level; for an active-low LED that is level 0.
    carrier.flags.polarity_active_low = active_high ? 0 : 1;
    rmt_copy_encoder_config_t enc_cfg = {};
    rmt_encoder_handle_t enc = nullptr;
    bool ok = rmt_apply_carrier(tx, &carrier) == ESP_OK && rmt_new_copy_encoder(&enc_cfg, &enc) == ESP_OK &&
              rmt_enable(tx) == ESP_OK;
    for (int r = 0; ok && r < repeat; ++r) {
        if (r)
            vTaskDelay(pdMS_TO_TICKS(40));
        rmt_encoder_reset(enc);
        rmt_transmit_config_t tcfg = {};
        tcfg.flags.eot_level = active_high ? 0 : 1;  // LED off when idle
        ok = rmt_transmit(tx, enc, symbols.data(), symbols.size() * sizeof(rmt_symbol_word_t), &tcfg) ==
                 ESP_OK &&
             rmt_tx_wait_all_done(tx, 3000) == ESP_OK;
    }
    rmt_disable(tx);
    if (enc)
        rmt_del_encoder(enc);
    rmt_del_channel(tx);
    // Leave the pin driving "LED off" so a floating line cannot light the LED.
    gpio_set_direction(static_cast<gpio_num_t>(tx_gpio), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(tx_gpio), active_high ? 0 : 1);
    return ok;
}

bool IrService::LoopbackHeard(int tx_gpio, int rx_gpio, bool active_high) {
    // NEC frame address 0x00, command 0x5A, used only as a probe.
    std::vector<uint16_t> t = {9000, 4500};
    auto byte = [&t](uint8_t b) {
        for (int i = 0; i < 8; ++i, b >>= 1) {
            t.push_back(560);
            t.push_back((b & 1) ? 1690 : 560);
        }
    };
    byte(0x00);
    byte(0xFF);
    byte(0x5A);
    byte(0xA5);
    t.push_back(560);

    std::vector<uint16_t> heard;
    {
        std::unique_lock<std::mutex> guard(rmt_mutex_);
        RxSession rx(rx_gpio);
        if (!rx.ok() || !rx.Arm())
            return false;
        guard.unlock();  // Send() takes the mutex itself; RX keeps running
        Send(tx_gpio, t.data(), t.size(), active_high, 38000, 1);
        guard.lock();
        if (rx.Wait(300)) {
            uint32_t dur = 0;
            heard = rx.Timings(&dur);
        }
    }
    // Accept when the leader and most bits came back.
    const bool ok = heard.size() >= 40 && heard[0] > 6000 && heard[0] < 12000;
    ESP_LOGI(TAG, "loopback tx=%d rx=%d %s: %u timings -> %s", tx_gpio, rx_gpio,
             active_high ? "active-high" : "active-low", unsigned(heard.size()), ok ? "heard" : "silent");
    return ok;
}

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
    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));
    std::string json = "{";
    for (size_t i = 0; i < n; ++i) {
        gpio_isr_handler_remove(ctx[i].gpio);
        gpio_reset_pin(ctx[i].gpio);
        char buf[32];
        snprintf(buf, sizeof(buf), "%s\"%d\":%u", i ? "," : "", gpios[i], (unsigned)ctx[i].edges);
        json += buf;
    }
    return json + "}";
}

std::string IrService::TimingsToJson(const std::vector<uint16_t>& timings) {
    std::string json = "[";
    for (size_t i = 0; i < timings.size(); ++i) {
        if (i)
            json += ",";
        json += std::to_string(timings[i]);
    }
    return json + "]";
}

std::string IrService::LearnFrame(int rx_gpio, int timeout_ms) {
    std::vector<uint16_t> t;
    std::string error;
    if (!Learn(rx_gpio, timeout_ms, &t, &error))
        return "{\"ok\":false,\"error\":\"" + error + "\"}";
    return "{\"ok\":true,\"count\":" + std::to_string(t.size()) + ",\"timings_us\":" + TimingsToJson(t) + "}";
}
