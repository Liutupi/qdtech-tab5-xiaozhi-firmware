#include "radio_service.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <iterator>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "cJSON.h"
#include "desktop_ui.h"
#include "tab5_sd.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "mp3dec.h"
#include "settings.h"
#include "tab5_audio_codec.h"
#include "wifi_manager.h"

#include <esp_timer.h>
#include <sys/socket.h>
#include <unistd.h>

static const char* TAG = "RadioService";

// Keep enough compressed audio in PSRAM to absorb Wi-Fi/TLS scheduling jitter.
// A 320 kbps stream consumes about 40 KB/s, so the steady-state target below
// provides roughly 600 ms of headroom without spending scarce internal SRAM.
static constexpr int kReadBufferSize = 48 * 1024;
static constexpr int kRadioReadTargetBytes = 32 * 1024;
static constexpr int kMusicReadTargetBytes = 32 * 1024;
static constexpr int kInitialReadTargetBytes = 12 * 1024;
static constexpr int kReadChunkBytes = 4096;
static constexpr int kPcmMaxSamples = MAX_NCHAN * MAX_NGRAN * MAX_NSAMP;
static constexpr int kPcmOutputMaxSamples = kPcmMaxSamples * 3;
static constexpr TickType_t kCustomUrlSpeakingGraceTicks = pdMS_TO_TICKS(4000);
static constexpr int kRadioEmptyReadLimit = 80;
static constexpr int kCustomUrlEmptyReadLimit = 400;
// Live streams can hang inside esp_http_client_read (TLS half-open) and never
// return; this watchdog closes the client so PlayUrl can reconnect.
// 15s: shorter windows kill healthy-but-jittery links (phone hotspots) and
// cause the audible stutter from constant reconnects.
static constexpr int64_t kReadStallTimeoutUs = 25LL * 1000000;
static std::atomic<esp_http_client_handle_t> g_active_stream_client{nullptr};
static std::atomic<int64_t> g_last_stream_progress_us{0};
// The timer callback runs on the ESP timer task. It may inspect/shutdown the
// socket, while only the radio task closes and frees the HTTP client.
static std::mutex g_stream_client_mutex;
static esp_timer_handle_t g_stream_stall_timer = nullptr;
static constexpr int kCustomUrlMaxReconnectAttempts = 3;

static void UpdateStreamProgress(esp_http_client_handle_t client, int64_t when_us) {
    std::lock_guard<std::mutex> lock(g_stream_client_mutex);
    if (g_active_stream_client.load(std::memory_order_relaxed) == client)
        g_last_stream_progress_us.store(when_us, std::memory_order_relaxed);
}

static void DetachStreamClient(esp_http_client_handle_t client) {
    std::lock_guard<std::mutex> lock(g_stream_client_mutex);
    if (g_active_stream_client.load(std::memory_order_relaxed) == client) {
        g_active_stream_client.store(nullptr, std::memory_order_relaxed);
        g_last_stream_progress_us.store(0, std::memory_order_relaxed);
    }
}

// ---- Background stream reader -------------------------------------------------------
// The HTTP read used to run inline with MP3 decode, so every slow read (Wi-Fi power save,
// a busy CDN) paused decoding and starved the speaker even when the compressed buffer
// was full. A separate task now pulls the stream into a large PSRAM ring while the radio
// task only decodes; network hiccups shorter than the ring are inaudible.
static constexpr size_t kMusicRingBytes[] = {384 * 1024, 192 * 1024, 96 * 1024};
static constexpr size_t kRadioRingBytes[] = {128 * 1024, 64 * 1024};

namespace {
struct StreamReader {
    esp_http_client_handle_t client = nullptr;
    StreamBufferHandle_t ring = nullptr;
    SemaphoreHandle_t done = nullptr;
    std::atomic<bool> stop{false};
    std::atomic<int> state{0};  // 0 running, 1 complete, 2 failed
    std::atomic<size_t> total{0};
    bool custom_url = false;
    int content_length = 0;
    int empty_limit = 80;
    std::string station_name;
    int url_index = 0;
};

void StreamReaderTask(void* arg) {
    auto* r = static_cast<StreamReader*>(arg);
    auto* chunk = static_cast<uint8_t*>(
        heap_caps_malloc(kReadChunkBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    int empty_reads = 0;
    if (!chunk)
        r->state.store(2);
    while (chunk && !r->stop.load()) {
        if (xStreamBufferSpacesAvailable(r->ring) < static_cast<size_t>(kReadChunkBytes)) {
            // Ring full: the stream is healthy, keep the stall watchdog quiet.
            UpdateStreamProgress(r->client, esp_timer_get_time());
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        // A finished download must not call read again: some CDNs keep the socket open
        // after the last byte, and the read would block until the stall watchdog fires.
        if (esp_http_client_is_complete_data_received(r->client) ||
            (r->content_length > 0 && r->total.load() >= static_cast<size_t>(r->content_length))) {
            r->state.store(1);
            break;
        }
        const int read =
            esp_http_client_read(r->client, reinterpret_cast<char*>(chunk), kReadChunkBytes);
        UpdateStreamProgress(r->client, esp_timer_get_time());
        if (read > 0) {
            empty_reads = 0;
            xStreamBufferSend(r->ring, chunk, read, portMAX_DELAY);
            r->total.fetch_add(read);
            continue;
        }
        if (read == 0) {
            if (esp_http_client_is_complete_data_received(r->client)) {
                r->state.store(1);
                break;
            }
            // Only give up when the decoder has nothing left to play.
            if (++empty_reads >= r->empty_limit && xStreamBufferIsEmpty(r->ring)) {
                ESP_LOGW(TAG, "stream stalled station=%s url_index=%d empty_reads=%d",
                         r->station_name.c_str(), r->url_index, empty_reads);
                r->state.store(2);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!r->stop.load())
            ESP_LOGW(TAG, "stream read failed station=%s url_index=%d read=%d",
                     r->station_name.c_str(), r->url_index, read);
        r->state.store(2);
        break;
    }
    if (chunk)
        heap_caps_free(chunk);
    // Nothing left to watch: the decoder may drain the ring for a while after this.
    UpdateStreamProgress(r->client, 0);
    xSemaphoreGive(r->done);
    // The radio task deletes this task (vTaskDeleteWithCaps from outside frees its TCB and
    // PSRAM stack); just park here until then.
    for (;;)
        vTaskSuspend(nullptr);
}

struct DeferredReader {
    StreamReader* reader = nullptr;
    TaskHandle_t task = nullptr;
    esp_http_client_handle_t client = nullptr;
};

// The radio task owns this fallback list. The normal path transfers timed-out
// readers to a low-priority reaper, so HTTP teardown cannot stall MP3 output.
static std::array<DeferredReader, 4> g_deferred_readers;
static size_t g_fallback_reader_count = 0;
// Includes queued, reaper-owned and fallback readers until cleanup completes.
static std::atomic<size_t> g_deferred_reader_count{0};
// Created by the radio task on the first deferred reader, then kept for life.
static QueueHandle_t g_deferred_reaper_queue = nullptr;

void DestroyStreamReader(StreamReader* reader, TaskHandle_t reader_task) {
    if (reader_task)
        vTaskDeleteWithCaps(reader_task);
    if (reader->ring) {
        uint8_t* storage = nullptr;
        StaticStreamBuffer_t* control = nullptr;
        if (xStreamBufferGetStaticBuffers(reader->ring, &storage, &control) == pdTRUE) {
            // IDF 6.0.2's vStreamBufferDeleteWithCaps frees this control block
            // twice. These two allocations belong to this reader instead.
            vStreamBufferDelete(reader->ring);
            heap_caps_free(storage);
            heap_caps_free(control);
        } else {
            vStreamBufferDelete(reader->ring);
        }
    }
    if (reader->done)
        vSemaphoreDelete(reader->done);
    delete reader;
}

void DeferredReaderReaperTask(void* arg) {
    auto queue = static_cast<QueueHandle_t>(arg);
    std::array<DeferredReader, 4> pending{};
    size_t pending_count = 0;
    for (;;) {
        DeferredReader incoming;
        if (pending_count < pending.size()) {
            const TickType_t wait = pending_count ? pdMS_TO_TICKS(100) : portMAX_DELAY;
            if (xQueueReceive(queue, &incoming, wait) == pdTRUE)
                pending[pending_count++] = incoming;
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        for (size_t i = 0; i < pending_count;) {
            const auto orphan = pending[i];
            if (xSemaphoreTake(orphan.reader->done, 0) != pdTRUE) {
                ++i;
                continue;
            }
            // The reader has signalled done and parked. It can no longer touch
            // its ring or HTTP handle, which the radio task already detached.
            DestroyStreamReader(orphan.reader, orphan.task);
            esp_http_client_close(orphan.client);
            esp_http_client_cleanup(orphan.client);
            pending[i] = pending[--pending_count];
            const size_t remaining = g_deferred_reader_count.fetch_sub(1) - 1;
            ESP_LOGI(TAG, "deferred stream reader reclaimed remaining=%u psram_free=%u",
                     static_cast<unsigned>(remaining),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        }
    }
}

bool EnsureDeferredReaderReaper() {
    if (g_deferred_reaper_queue)
        return true;
    auto queue = xQueueCreate(g_deferred_readers.size(), sizeof(DeferredReader));
    if (!queue)
        return false;
    TaskHandle_t task = nullptr;
    // The reader and Wi-Fi tasks have higher priority on core 0; MP3 decode is
    // pinned to core 1. Keep HTTP close and heap teardown off the audio core.
    const BaseType_t created =
        xTaskCreatePinnedToCoreWithCaps(DeferredReaderReaperTask, "radio_reaper", 6144, queue, 1,
                                        &task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        vQueueDelete(queue);
        return false;
    }
    g_deferred_reaper_queue = queue;
    return true;
}

bool DeferStreamReader(const DeferredReader& orphan) {
    if (g_deferred_reader_count.load() >= g_deferred_readers.size())
        return false;
    g_deferred_reader_count.fetch_add(1);
    if (EnsureDeferredReaderReaper() && xQueueSend(g_deferred_reaper_queue, &orphan, 0) == pdTRUE)
        return true;
    // If the cleanup task cannot start, preserve the existing safe fallback.
    if (g_fallback_reader_count < g_deferred_readers.size()) {
        g_deferred_readers[g_fallback_reader_count++] = orphan;
        ESP_LOGW(TAG, "radio reaper unavailable; deferred cleanup on player task");
        return true;
    }
    g_deferred_reader_count.fetch_sub(1);
    return false;
}

void ReapFallbackReaders() {
    for (size_t i = 0; i < g_fallback_reader_count;) {
        const auto orphan = g_deferred_readers[i];
        if (xSemaphoreTake(orphan.reader->done, 0) != pdTRUE) {
            ++i;
            continue;
        }
        // The reader has signalled done and parked. Delete its task first,
        // then its ring/semaphore, then the HTTP handle it no longer touches.
        DestroyStreamReader(orphan.reader, orphan.task);
        esp_http_client_close(orphan.client);
        esp_http_client_cleanup(orphan.client);
        g_deferred_readers[i] = g_deferred_readers[--g_fallback_reader_count];
        const size_t remaining = g_deferred_reader_count.fetch_sub(1) - 1;
        ESP_LOGI(TAG, "deferred stream reader reclaimed remaining=%u",
                 static_cast<unsigned>(remaining));
    }
}
}  // namespace
static constexpr int kMinimumCustomMusicBytes = 1024 * 1024;

enum class RadioCategory {
    NATIONAL,    // 全国
    BEIJING,     // 北京
    SHANGHAI,    // 上海
    GUANGDONG,   // 广东
    ZHEJIANG,    // 浙江
    JIANGSU,     // 江苏
    SICHUAN,     // 四川
    HUNAN,       // 湖南
    HUBEI,       // 湖北
    SHANDONG,    // 山东
    MUSIC,       // 音乐
    TRAFFIC,     // 交通
    OTHER,       // 其他
};

struct RadioStation {
    std::string name;
    std::string urls[3];
    std::string codec;
    int bitrate_kbps;
    RadioCategory category;
    bool favorite;
};

static std::vector<RadioStation> kStations;
static std::mutex g_catalog_mutex;
static std::mutex g_favorites_mutex;
static std::mutex g_favorites_save_mutex;

static std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

static bool IsNetEaseMusicUrl(const std::string& url) {
    return url.find(".music.126.net/") != std::string::npos ||
           url.find("music.163.com/") != std::string::npos;
}

static RadioCategory ParseCategory(const char* cat) {
    if (!cat) return RadioCategory::OTHER;
    std::string c = Lower(cat);
    if (c == "national" || c == "全国") return RadioCategory::NATIONAL;
    if (c == "beijing" || c == "北京") return RadioCategory::BEIJING;
    if (c == "shanghai" || c == "上海") return RadioCategory::SHANGHAI;
    if (c == "guangdong" || c == "广东") return RadioCategory::GUANGDONG;
    if (c == "zhejiang" || c == "浙江") return RadioCategory::ZHEJIANG;
    if (c == "jiangsu" || c == "江苏") return RadioCategory::JIANGSU;
    if (c == "sichuan" || c == "四川") return RadioCategory::SICHUAN;
    if (c == "hunan" || c == "湖南") return RadioCategory::HUNAN;
    if (c == "hubei" || c == "湖北") return RadioCategory::HUBEI;
    if (c == "shandong" || c == "山东") return RadioCategory::SHANDONG;
    if (c == "music" || c == "音乐") return RadioCategory::MUSIC;
    if (c == "traffic" || c == "交通") return RadioCategory::TRAFFIC;
    return RadioCategory::OTHER;
}

static void LoadBuiltinStations() {
    if (!kStations.empty()) return;

    struct BuiltinStation {
        const char* name;
        const char* urls[3];
        const char* codec;
        int bitrate_kbps;
        RadioCategory category;
    };

    // Probed 2026-09 with GET → Content-Type audio/mpeg. Dual CN mirrors where
    // available. Station names avoid CJK outside font_symbols.txt. International
    // channels use ASCII so the LXGW subset can render them.
    static const BuiltinStation builtin[] = {
        // —— 全国 / 新闻 ——
        {"中国之声", {"https://lhttp.qtfm.cn/live/15318317/64k.mp3", "https://lhttp-hw.qtfm.cn/live/15318317/64k.mp3", "http://lhttp.qingting.fm/live/15318317/64k.mp3"}, "MP3", 64, RadioCategory::NATIONAL},
        {"财经之声", {"https://lhttp.qtfm.cn/live/15318569/64k.mp3", "https://lhttp-hw.qtfm.cn/live/15318569/64k.mp3", "http://lhttp.qingting.fm/live/15318569/64k.mp3"}, "MP3", 64, RadioCategory::NATIONAL},
        // —— 北京 ——
        {"北京新闻广播", {"https://lhttp.qtfm.cn/live/339/64k.mp3", "https://lhttp.qingting.fm/live/339/64k.mp3", nullptr}, "MP3", 64, RadioCategory::BEIJING},
        {"北京交通广播", {"https://lhttp.qtfm.cn/live/336/64k.mp3", "https://lhttp.qingting.fm/live/336/64k.mp3", nullptr}, "MP3", 64, RadioCategory::BEIJING},
        {"北京音乐广播", {"https://lhttp.qtfm.cn/live/4938/64k.mp3", "https://lhttp.qingting.fm/live/4938/64k.mp3", nullptr}, "MP3", 64, RadioCategory::BEIJING},
        // —— 上海 ——
        {"上海新闻广播", {"https://lhttp.qtfm.cn/live/1259/64k.mp3", "https://lhttp.qingting.fm/live/1259/64k.mp3", nullptr}, "MP3", 64, RadioCategory::SHANGHAI},
        {"上海交通广播", {"https://lhttp.qtfm.cn/live/1260/64k.mp3", "https://lhttp.qingting.fm/live/1260/64k.mp3", nullptr}, "MP3", 64, RadioCategory::SHANGHAI},
        {"上海动感音乐", {"https://lhttp.qtfm.cn/live/1271/64k.mp3", "https://lhttp.qingting.fm/live/1271/64k.mp3", nullptr}, "MP3", 64, RadioCategory::SHANGHAI},
        // —— 广东 ——
        {"广州新闻台", {"https://lhttp.qtfm.cn/live/4848/64k.mp3", "https://lhttp.qingting.fm/live/4848/64k.mp3", nullptr}, "MP3", 64, RadioCategory::GUANGDONG},
        {"广州交通电台", {"https://lhttp.qtfm.cn/live/4955/64k.mp3", "https://lhttp.qingting.fm/live/4955/64k.mp3", nullptr}, "MP3", 64, RadioCategory::GUANGDONG},
        {"广东新闻广播", {"https://lhttp.qtfm.cn/live/1254/64k.mp3", "https://lhttp.qingting.fm/live/1254/64k.mp3", nullptr}, "MP3", 64, RadioCategory::GUANGDONG},
        {"广东交通之声", {"https://lhttp.qtfm.cn/live/1262/64k.mp3", "https://lhttp.qingting.fm/live/1262/64k.mp3", nullptr}, "MP3", 64, RadioCategory::GUANGDONG},
        {"广东文体广播", {"https://lhttp.qtfm.cn/live/471/64k.mp3", "https://lhttp.qingting.fm/live/471/64k.mp3", nullptr}, "MP3", 64, RadioCategory::GUANGDONG},
        // —— 浙江 / 江苏 / 西南 / 华中 ——
        {"浙江之声", {"https://lhttp.qtfm.cn/live/1223/64k.mp3", "https://lhttp.qingting.fm/live/1223/64k.mp3", nullptr}, "MP3", 64, RadioCategory::ZHEJIANG},
        {"浙江交通广播", {"https://lhttp.qtfm.cn/live/5021381/64k.mp3", "https://lhttp.qingting.fm/live/5021381/64k.mp3", nullptr}, "MP3", 64, RadioCategory::ZHEJIANG},
        {"浙江音乐广播", {"https://lhttp.qtfm.cn/live/5022107/64k.mp3", "https://lhttp.qingting.fm/live/5022107/64k.mp3", nullptr}, "MP3", 64, RadioCategory::ZHEJIANG},
        {"江苏新闻广播", {"https://lhttp.qtfm.cn/live/5022308/64k.mp3", "https://lhttp.qingting.fm/live/5022308/64k.mp3", nullptr}, "MP3", 64, RadioCategory::JIANGSU},
        {"江苏交通广播", {"https://lhttp.qtfm.cn/live/4915/64k.mp3", "https://lhttp.qingting.fm/live/4915/64k.mp3", nullptr}, "MP3", 64, RadioCategory::JIANGSU},
        {"四川新闻广播", {"https://lhttp.qtfm.cn/live/1225/64k.mp3", "https://lhttp.qingting.fm/live/1225/64k.mp3", nullptr}, "MP3", 64, RadioCategory::SICHUAN},
        {"楚天交通广播", {"https://lhttp.qtfm.cn/live/1233/64k.mp3", "https://lhttp.qingting.fm/live/1233/64k.mp3", nullptr}, "MP3", 64, RadioCategory::HUBEI},
        {"动听音乐台", {"https://lhttp.qtfm.cn/live/5022107/64k.mp3", "https://lhttp.qingting.fm/live/5022107/64k.mp3", nullptr}, "MP3", 64, RadioCategory::MUSIC},
        // —— 海外 MP3 直播（128kbps，需外网）——
        {"SomaFM GrooveSalad", {"https://ice1.somafm.com/groovesalad-128-mp3", nullptr, nullptr}, "MP3", 128, RadioCategory::MUSIC},
        {"SomaFM IndiePop", {"https://ice1.somafm.com/indiepop-128-mp3", nullptr, nullptr}, "MP3", 128, RadioCategory::MUSIC},
        {"SomaFM SecretAgent", {"https://ice1.somafm.com/secretagent-128-mp3", nullptr, nullptr}, "MP3", 128, RadioCategory::MUSIC},
        {"SomaFM SpaceStation", {"https://ice1.somafm.com/spacestation-128-mp3", nullptr, nullptr}, "MP3", 128, RadioCategory::MUSIC},
        {"SomaFM Fluid", {"https://ice1.somafm.com/fluid-128-mp3", nullptr, nullptr}, "MP3", 128, RadioCategory::MUSIC},
        {"SomaFM Metal", {"https://ice1.somafm.com/metal-128-mp3", nullptr, nullptr}, "MP3", 128, RadioCategory::MUSIC},
        {"FIP France Music", {"https://icecast.radiofrance.fr/fip-midfi.mp3", nullptr, nullptr}, "MP3", 128, RadioCategory::MUSIC},
        {"France Info", {"https://icecast.radiofrance.fr/franceinfo-midfi.mp3", nullptr, nullptr}, "MP3", 128, RadioCategory::NATIONAL},
        {"WNYC New York", {"https://fm939.wnyc.org/wnycfm", nullptr, nullptr}, "MP3", 128, RadioCategory::OTHER},
        {"KEXP Seattle", {"https://kexp-mp3-128.streamguys1.com/kexp128.mp3", nullptr, nullptr}, "MP3", 128, RadioCategory::MUSIC},
        {"KUSC Classic", {"https://playerservices.streamtheworld.com/api/livestream-redirect/KUSCMP128.mp3", nullptr, nullptr}, "MP3", 128, RadioCategory::MUSIC},
        {"Swiss Jazz", {"https://stream.srg-ssr.ch/m/rsj/mp3_128", nullptr, nullptr}, "MP3", 128, RadioCategory::MUSIC},
        {"Swiss Classic", {"https://stream.srg-ssr.ch/m/rsc_de/mp3_128", nullptr, nullptr}, "MP3", 128, RadioCategory::MUSIC},
        {"Swiss Pop", {"https://stream.srg-ssr.ch/m/rsp/mp3_128", nullptr, nullptr}, "MP3", 128, RadioCategory::MUSIC},
        {"DLF News", {"https://st01.sslstream.dlf.de/dlf/01/128/mp3/stream.mp3", nullptr, nullptr}, "MP3", 128, RadioCategory::NATIONAL},
        {"DLF Nova", {"https://st03.sslstream.dlf.de/dlf/03/128/mp3/stream.mp3", nullptr, nullptr}, "MP3", 128, RadioCategory::MUSIC},
    };

    for (const auto& s : builtin) {
        RadioStation station;
        station.name = s.name;
        station.urls[0] = s.urls[0] ? s.urls[0] : "";
        station.urls[1] = s.urls[1] ? s.urls[1] : "";
        station.urls[2] = s.urls[2] ? s.urls[2] : "";
        station.codec = s.codec;
        station.bitrate_kbps = s.bitrate_kbps;
        station.category = s.category;
        station.favorite = false;
        kStations.push_back(station);
    }
    ESP_LOGI(TAG, "Loaded %d builtin stations", static_cast<int>(kStations.size()));
}

static bool EnsureSdCardMounted() {
    return Tab5SdReady();
}

static bool LoadStationsFromSdCard() {
    if (!EnsureSdCardMounted()) {
        ESP_LOGI(TAG, "SD card not available, using built-in stations");
        return false;
    }
    
    const char* path = "/sdcard/radio.json";
    FILE* file = fopen(path, "rb");
    if (!file) {
        ESP_LOGI(TAG, "radio.json not found on SD card, using built-in stations");
        return false;
    }
    
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    
    if (size <= 0 || size > 32768) {
        fclose(file);
        ESP_LOGW(TAG, "radio.json invalid size: %ld", size);
        return false;
    }
    
    char* buffer = static_cast<char*>(heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buffer) {
        fclose(file);
        ESP_LOGW(TAG, "radio.json alloc failed");
        return false;
    }
    
    size_t read = fread(buffer, 1, size, file);
    fclose(file);
    buffer[read] = '\0';
    
    cJSON* root = cJSON_Parse(buffer);
    heap_caps_free(buffer);
    
    if (!root) {
        ESP_LOGW(TAG, "radio.json parse failed");
        return false;
    }
    
    cJSON* stations = cJSON_GetObjectItem(root, "stations");
    if (!cJSON_IsArray(stations)) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "radio.json missing stations array");
        return false;
    }
    
    kStations.clear();
    int count = cJSON_GetArraySize(stations);
    for (int i = 0; i < count && i < 128; ++i) {
        cJSON* item = cJSON_GetArrayItem(stations, i);
        if (!item) continue;
        
        cJSON* name = cJSON_GetObjectItem(item, "name");
        cJSON* url = cJSON_GetObjectItem(item, "url");
        cJSON* url2 = cJSON_GetObjectItem(item, "url2");
        cJSON* url3 = cJSON_GetObjectItem(item, "url3");
        cJSON* codec = cJSON_GetObjectItem(item, "codec");
        cJSON* bitrate = cJSON_GetObjectItem(item, "bitrate");
        cJSON* category = cJSON_GetObjectItem(item, "category");
        
        if (!cJSON_IsString(name) || !cJSON_IsString(url)) continue;
        
        RadioStation station;
        station.name = name->valuestring;
        station.urls[0] = url->valuestring;
        station.urls[1] = cJSON_IsString(url2) ? url2->valuestring : "";
        station.urls[2] = cJSON_IsString(url3) ? url3->valuestring : "";
        station.codec = cJSON_IsString(codec) ? codec->valuestring : "MP3";
        station.bitrate_kbps = cJSON_IsNumber(bitrate) ? bitrate->valueint : 64;
        station.category = ParseCategory(cJSON_IsString(category) ? category->valuestring : nullptr);
        station.favorite = false;
        kStations.push_back(station);
    }
    
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d stations from radio.json", (int)kStations.size());
    return !kStations.empty();
}

static void EnsureStationsLoaded() {
    static std::once_flag loaded;
    std::call_once(loaded, [] {
        if (!LoadStationsFromSdCard())
            LoadBuiltinStations();
        // Keep catalog name c_str() pointers stable when Start appends its
        // reserved music slot.
        kStations.reserve(kStations.size() + 1);
    });
}

static int StationCount() {
    EnsureStationsLoaded();
    return static_cast<int>(kStations.size());
}

static const char* CategoryName(RadioCategory category) {
    switch (category) {
        case RadioCategory::NATIONAL: return "全国";
        case RadioCategory::BEIJING: return "北京";
        case RadioCategory::SHANGHAI: return "上海";
        case RadioCategory::GUANGDONG: return "广东";
        case RadioCategory::ZHEJIANG: return "浙江";
        case RadioCategory::JIANGSU: return "江苏";
        case RadioCategory::SICHUAN: return "四川";
        case RadioCategory::HUNAN: return "湖南";
        case RadioCategory::HUBEI: return "湖北";
        case RadioCategory::SHANDONG: return "山东";
        case RadioCategory::MUSIC: return "音乐";
        case RadioCategory::TRAFFIC: return "交通";
        case RadioCategory::OTHER: return "其他";
        default: return "未知";
    }
}

static std::vector<int> GetStationsByCategory(RadioCategory category, int catalog_count) {
    std::vector<int> result;
    for (int i = 0; i < catalog_count; ++i) {
        if (kStations[i].category == category) {
            result.push_back(i);
        }
    }
    return result;
}

static std::vector<int> GetFavoriteStations(int catalog_count) {
    std::vector<int> result;
    std::lock_guard<std::mutex> lock(g_favorites_mutex);
    for (int i = 0; i < catalog_count; ++i) {
        if (kStations[i].favorite) {
            result.push_back(i);
        }
    }
    return result;
}

static int16_t Clamp16(int value) {
    if (value > 32767) {
        return 32767;
    }
    if (value < -32768) {
        return -32768;
    }
    return static_cast<int16_t>(value);
}

void RadioService::Start(DesktopUI* desktop_ui, StateCallback callback) {
    std::lock_guard<std::mutex> start_lock(start_mutex_);
    if (started_) return;
    desktop_ui_ = desktop_ui;
    state_callback_ = std::move(callback);
    
    {
        std::lock_guard<std::mutex> lock(g_catalog_mutex);
        catalog_station_count_ = StationCount();
        // The catalog never reallocates or changes its strings once the task
        // is running. Only the radio task writes this reserved music slot.
        custom_station_index_ = catalog_station_count_;
        RadioStation custom_station;
        custom_station.name = "Music URL";
        custom_station.codec = "MP3";
        custom_station.bitrate_kbps = 0;
        custom_station.category = RadioCategory::MUSIC;
        custom_station.favorite = false;
        kStations.push_back(std::move(custom_station));
    }
    const int count = StationCount();
    ESP_LOGI(TAG, "RadioService::Start: %d catalog stations loaded", catalog_station_count_);
    
    last_success_url_.resize(count, -1);
    audio_focus_blocked_.store(IsXiaozhiAudioState(), std::memory_order_relaxed);
    queue_ = xQueueCreate(16, sizeof(CommandMessage));
    if (!queue_) {
        ESP_LOGE(TAG, "radio queue create failed free_internal=%u largest_internal=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        started_ = false;
        {
            std::lock_guard<std::mutex> lock(g_catalog_mutex);
            kStations.pop_back();
        }
        custom_station_index_ = -1;
        catalog_station_count_ = 0;
        SetUi("Unavailable", "No memory");
        return;
    }
    LoadFavorites();
    LoadStationIndex();
    PublishCurrentStation();
    
    constexpr uint32_t kTaskStackBytes = 6144;
    ESP_LOGI(TAG, "radio task create free_internal=%u largest_internal=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    // Keep decode/I2S feeding ahead of LVGL's full-screen RGB transfers.  At
    // the same priority an occasional 40-50 ms display flush can starve the
    // codec long enough to make high-bitrate music audibly stutter.
    constexpr UBaseType_t kAudioTaskPriority = 6;
    constexpr BaseType_t kAudioTaskCore = 1;
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps([](void* arg) {
        static_cast<RadioService*>(arg)->Task();
    }, "radio_service", kTaskStackBytes, this, kAudioTaskPriority, &task_handle_, kAudioTaskCore,
       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    task_stack_internal_ = false;
    if (ret != pdPASS) {
        ESP_LOGW(TAG, "radio PSRAM task create failed ret=%ld, trying internal memory",
                 static_cast<long>(ret));
        task_handle_ = nullptr;
        ret = xTaskCreatePinnedToCore([](void* arg) {
            static_cast<RadioService*>(arg)->Task();
        }, "radio_service", kTaskStackBytes, this, kAudioTaskPriority, &task_handle_, kAudioTaskCore);
        task_stack_internal_ = (ret == pdPASS);
    }
    if (ret != pdPASS) {
        auto queue = static_cast<QueueHandle_t>(queue_);
        if (queue) {
            vQueueDelete(queue);
        }
        queue_ = nullptr;
        task_handle_ = nullptr;
        started_ = false;
        {
            std::lock_guard<std::mutex> lock(g_catalog_mutex);
            kStations.pop_back();
        }
        custom_station_index_ = -1;
        catalog_station_count_ = 0;
        ESP_LOGE(TAG, "radio task create failed ret=%ld free_internal=%u largest_internal=%u",
                 static_cast<long>(ret),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        SetUi("Unavailable", "No memory");
        return;
    }
    ESP_LOGI(TAG, "radio task started stack=%u memory=%s",
             static_cast<unsigned>(kTaskStackBytes),
             task_stack_internal_ ? "internal" : "psram");

    Application::GetInstance().RegisterDeviceStateCallback([this](DeviceState previous, DeviceState current) {
        OnDeviceStateChanged(static_cast<int>(previous), static_cast<int>(current));
    });
    started_.store(true, std::memory_order_release);
    SetUi("Ready", "Tap Play");
}

void RadioService::LoadFavorites() {
    Settings settings("radio_fav", false);
    std::lock_guard<std::mutex> lock(g_favorites_mutex);
    for (int i = 0; i < catalog_station_count_; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "fav_%d", i);
        kStations[i].favorite = settings.GetInt(key, 0) == 1;
    }
}

void RadioService::SaveFavorites() {
    std::lock_guard<std::mutex> save_lock(g_favorites_save_mutex);
    std::vector<bool> favorites;
    {
        std::lock_guard<std::mutex> catalog_lock(g_catalog_mutex);
        std::lock_guard<std::mutex> lock(g_favorites_mutex);
        const int count = catalog_station_count_ > 0 ? catalog_station_count_ : StationCount();
        favorites.reserve(count);
        for (int i = 0; i < count; ++i)
            favorites.push_back(kStations[i].favorite);
    }
    Settings settings("radio_fav", true);
    for (int i = 0; i < static_cast<int>(favorites.size()); ++i) {
        char key[16];
        snprintf(key, sizeof(key), "fav_%d", i);
        settings.SetInt(key, favorites[i] ? 1 : 0);
    }
}

void RadioService::LoadStationIndex() {
    Settings settings("radio_st", false);
    int index = settings.GetInt("last", 0);
    int count = catalog_station_count_;
    if (count > 0 && index >= 0 && index < count) {
        station_index_ = index;
        ESP_LOGI(TAG, "Loaded last station index=%d name=%s", station_index_, kStations[station_index_].name.c_str());
    } else {
        station_index_ = 0;
        ESP_LOGW(TAG, "Invalid station index %d, reset to 0 (count=%d)", index, count);
    }
}

void RadioService::SaveStationIndex() {
    if (!task_stack_internal_ && xTaskGetCurrentTaskHandle() == task_handle_) {
        const int index = station_index_;
        Application::GetInstance().Schedule([index] {
            Settings settings("radio_st", true);
            settings.SetInt("last", index);
        });
        return;
    }
    Settings settings("radio_st", true);
    settings.SetInt("last", station_index_);
    ESP_LOGI(TAG, "Saved station index=%d", station_index_);
}

void RadioService::ToggleFavorite(int index) {
    {
        std::lock_guard<std::mutex> catalog_lock(g_catalog_mutex);
        const int count = catalog_station_count_ > 0 ? catalog_station_count_ : StationCount();
        if (index < 0 || index >= count) return;
        std::lock_guard<std::mutex> lock(g_favorites_mutex);
        kStations[index].favorite = !kStations[index].favorite;
    }
    SaveFavorites();
}

bool RadioService::IsFavorite(int index) const {
    std::lock_guard<std::mutex> catalog_lock(g_catalog_mutex);
    const int count = catalog_station_count_ > 0 ? catalog_station_count_ : StationCount();
    if (index >= 0 && index < count) {
        std::lock_guard<std::mutex> lock(g_favorites_mutex);
        return kStations[index].favorite;
    }
    return false;
}

std::vector<int> RadioService::GetFavorites() const {
    std::lock_guard<std::mutex> lock(g_catalog_mutex);
    const int count = catalog_station_count_ > 0 ? catalog_station_count_ : StationCount();
    return GetFavoriteStations(count);
}

std::vector<int> RadioService::GetByCategory(int category) const {
    std::lock_guard<std::mutex> lock(g_catalog_mutex);
    const int count = catalog_station_count_ > 0 ? catalog_station_count_ : StationCount();
    return GetStationsByCategory(static_cast<RadioCategory>(category), count);
}

int RadioService::GetStationCount() const {
    std::lock_guard<std::mutex> lock(g_catalog_mutex);
    return catalog_station_count_ > 0 ? catalog_station_count_ : StationCount();
}

const char* RadioService::GetStationName(int index) const {
    std::lock_guard<std::mutex> lock(g_catalog_mutex);
    const int count = catalog_station_count_ > 0 ? catalog_station_count_ : StationCount();
    if (index >= 0 && index < count) {
        return kStations[index].name.c_str();
    }
    if (custom_station_index_ >= 0 && index == custom_station_index_) return "Music URL";
    return "";
}

const char* RadioService::GetStationCategory(int index) const {
    std::lock_guard<std::mutex> lock(g_catalog_mutex);
    const int count = catalog_station_count_ > 0 ? catalog_station_count_ : StationCount();
    if (index >= 0 && index < count) {
        return CategoryName(kStations[index].category);
    }
    if (custom_station_index_ >= 0 && index == custom_station_index_)
        return CategoryName(RadioCategory::MUSIC);
    return "";
}

int RadioService::GetStationCategoryId(int index) const {
    std::lock_guard<std::mutex> lock(g_catalog_mutex);
    const int count = catalog_station_count_ > 0 ? catalog_station_count_ : StationCount();
    if (index >= 0 && index < count) {
        return static_cast<int>(kStations[index].category);
    }
    if (custom_station_index_ >= 0 && index == custom_station_index_)
        return static_cast<int>(RadioCategory::MUSIC);
    return -1;
}

void RadioService::SelectStationIndex(int index, int category_filter) {
    std::lock_guard<std::mutex> focus_lock(audio_focus_mutex_);
    std::lock_guard<std::mutex> submission_lock(submission_mutex_);
    const int count = GetStationCount();
    if (index < 0 || index >= count) {
        ESP_LOGW(TAG, "SelectStationIndex ignored invalid index=%d count=%d", index, count);
        return;
    }
    replacement_pending_.store(true, std::memory_order_release);
    playback_release_pending_.store(false, std::memory_order_relaxed);
    stream_generation_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_station_index_ = index;
        pending_category_filter_ = category_filter;
        pending_custom_valid_ = false;
        pending_navigation_steps_ = 0;
    }
    music_playback_state_.store(0, std::memory_order_relaxed);
    PostCommand(Command::SELECT_STATION);
}

void RadioService::PlayPause() {
    std::lock_guard<std::mutex> submission_lock(submission_mutex_);
    if (replacement_pending_.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_custom_valid_ = false;
            pending_station_index_ = -1;
            pending_navigation_steps_ = 0;
        }
        replacement_pending_.store(false, std::memory_order_release);
        play_requested_ = false;
        stop_requested_ = true;
        stream_generation_.fetch_add(1, std::memory_order_relaxed);
        PostCommand(Command::PAUSE);
        return;
    }
    ESP_LOGI(TAG, "PlayPause requested play_requested=%d", play_requested_.load(std::memory_order_relaxed));
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_navigation_steps_ = 0;
    }
    stream_generation_.fetch_add(1, std::memory_order_relaxed);
    pending_toggle_parity_ = !pending_toggle_parity_;
    PostCommand(Command::PLAY_PAUSE);
}

void RadioService::Play() {
    std::lock_guard<std::mutex> submission_lock(submission_mutex_);
    bool selection_pending;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        selection_pending = pending_station_index_ >= 0 || pending_custom_valid_;
    }
    if (!play_requested_ && !selection_pending) {
        ESP_LOGI(TAG, "Play requested (was stopped)");
        pending_toggle_parity_ = true;
        PostCommand(Command::PLAY_PAUSE);
    } else {
        ESP_LOGI(TAG, "Play requested (already playing, refreshing focus)");
        PostCommand(Command::FOCUS_CHANGED);
    }
}

void RadioService::Pause() {
    std::lock_guard<std::mutex> submission_lock(submission_mutex_);
    bool pending_play;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_play = pending_custom_valid_ || pending_station_index_ >= 0;
        pending_custom_valid_ = false;
        pending_station_index_ = -1;
        pending_navigation_steps_ = 0;
    }
    replacement_pending_.store(false, std::memory_order_release);
    // A play command may be queued even while the published flag is still false.
    // Always enqueue PAUSE so a rapid play/pause tap cannot start playback later.
    ESP_LOGI(TAG, "Pause requested%s", (!play_requested_ && !pending_play) ? " (already paused)" : "");
    music_playback_state_.store(0, std::memory_order_relaxed);
    play_requested_ = false;
    stop_requested_ = true;
    stream_generation_.fetch_add(1, std::memory_order_relaxed);
    PostCommand(Command::PAUSE);
}

void RadioService::Stop() {
    std::lock_guard<std::mutex> submission_lock(submission_mutex_);
    ESP_LOGI(TAG, "Stop requested");
    music_playback_state_.store(0, std::memory_order_relaxed);
    playback_release_pending_.store(true, std::memory_order_relaxed);
    play_requested_ = false;
    stop_requested_ = true;
    stream_generation_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_custom_valid_ = false;
        pending_station_index_ = -1;
        pending_navigation_steps_ = 0;
    }
    replacement_pending_.store(false, std::memory_order_release);
    PostCommand(Command::STOP);
}

void RadioService::SetPlaybackReleasedCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(playback_callback_mutex_);
    playback_released_callback_ = std::move(callback);
}

void RadioService::NotifyPlaybackReleased() {
    ESP_LOGI(TAG, "playback resources released free_internal=%u largest_internal=%u minimum_internal=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(playback_callback_mutex_);
        callback = playback_released_callback_;
    }
    if (callback) callback();
}

void RadioService::Next() {
    std::lock_guard<std::mutex> submission_lock(submission_mutex_);
    music_playback_state_.store(0, std::memory_order_relaxed);
    stream_generation_.fetch_add(1, std::memory_order_relaxed);
    bool cancelled_pending_music = false;
    bool advanced_pending_station = false;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        cancelled_pending_music = pending_custom_valid_;
        pending_custom_valid_ = false;
        if (!cancelled_pending_music && pending_station_index_ >= 0) {
            pending_station_index_ = AdjacentCatalogIndex(pending_station_index_, 1, pending_category_filter_);
            pending_navigation_steps_ = 0;
            advanced_pending_station = true;
        } else {
            pending_station_index_ = -1;
            pending_navigation_steps_ = cancelled_pending_music ? 0 :
                std::min(8, pending_navigation_steps_ + 1);
            replacement_pending_.store(false, std::memory_order_release);
        }
        if (cancelled_pending_music) {
            // The player has not consumed the new song yet. Do not interpret
            // this tap as a request to start a radio station instead.
            play_requested_ = false;
            stop_requested_ = true;
            playback_release_pending_.store(true, std::memory_order_relaxed);
        }
        PostCommand(cancelled_pending_music ? Command::STOP :
                    advanced_pending_station ? Command::SELECT_STATION : Command::NEXT);
    }
}

void RadioService::Prev() {
    std::lock_guard<std::mutex> submission_lock(submission_mutex_);
    music_playback_state_.store(0, std::memory_order_relaxed);
    stream_generation_.fetch_add(1, std::memory_order_relaxed);
    bool cancelled_pending_music = false;
    bool advanced_pending_station = false;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        cancelled_pending_music = pending_custom_valid_;
        pending_custom_valid_ = false;
        if (!cancelled_pending_music && pending_station_index_ >= 0) {
            pending_station_index_ = AdjacentCatalogIndex(pending_station_index_, -1, pending_category_filter_);
            pending_navigation_steps_ = 0;
            advanced_pending_station = true;
        } else {
            pending_station_index_ = -1;
            pending_navigation_steps_ = cancelled_pending_music ? 0 :
                std::max(-8, pending_navigation_steps_ - 1);
            replacement_pending_.store(false, std::memory_order_release);
        }
        if (cancelled_pending_music) {
            play_requested_ = false;
            stop_requested_ = true;
            playback_release_pending_.store(true, std::memory_order_relaxed);
        }
        PostCommand(cancelled_pending_music ? Command::STOP :
                    advanced_pending_station ? Command::SELECT_STATION : Command::PREV);
    }
}

std::string RadioService::GetStatusJson() const {
    std::string name;
    std::string codec;
    int bitrate;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        name = published_station_name_;
        codec = published_station_codec_;
        bitrate = published_station_bitrate_;
    }
    cJSON* root = cJSON_CreateObject();
    if (!root) return "{}";
    cJSON_AddStringToObject(root, "station", name.c_str());
    cJSON_AddStringToObject(root, "state", play_requested_.load() ? "playing" : "stopped");
    cJSON_AddStringToObject(root, "codec", codec.c_str());
    cJSON_AddNumberToObject(root, "bitrate_kbps", bitrate);
    char* encoded = cJSON_PrintUnformatted(root);
    std::string result = encoded ? encoded : "{}";
    cJSON_free(encoded);
    cJSON_Delete(root);
    return result;
}

std::string RadioService::GetMusicStatusJson() const {
    const char* states[] = {"stopped", "playing", "ended", "unavailable"};
    const int state = music_playback_state_.load(std::memory_order_relaxed);
    return std::string("{\"state\":\"") + states[state >= 0 && state < 4 ? state : 0] + "\"}";
}

std::string RadioService::SelectStation(const std::string& station) {
    std::string needle = Lower(station);
    int selected_index = -1;
    std::string selected;
    {
        std::lock_guard<std::mutex> lock(g_catalog_mutex);
        const int count = catalog_station_count_ > 0 ? catalog_station_count_ : StationCount();
        for (int i = 0; i < count; ++i) {
            if (Lower(kStations[i].name).find(needle) != std::string::npos) {
                selected_index = i;
                selected = kStations[i].name;
                break;
            }
        }
    }
    if (selected_index < 0) return "Radio station not found.";
    // MCP selection historically cycled the full catalog afterwards.
    SelectStationIndex(selected_index, -1);
    return std::string("Radio station selected: ") + selected;
}

std::string RadioService::PlayUrlFromTool(const std::string& title, const std::string& artist, const std::string& url) {
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        return "Music URL was NOT started: 没有拿到可直接播放的歌曲链接。请重新搜索完整 MP3 直链，不要只返回歌名、网页、歌单或空链接。";
    }

    std::string display_name = title.empty() ? "Music URL" : title;
    if (!artist.empty()) {
        display_name += " - ";
        display_name += artist;
    }
    const std::string result = std::string("Music URL started on device; no spoken follow-up is needed: ") + display_name;

    std::lock_guard<std::mutex> focus_lock(audio_focus_mutex_);
    {
        std::lock_guard<std::mutex> submission_lock(submission_mutex_);
        // Mark and publish the replacement before interrupting the old stream.
        // The focus lock keeps a concurrent final release from racing Prepare.
        replacement_pending_.store(true, std::memory_order_release);
        playback_release_pending_.store(false, std::memory_order_relaxed);
        stream_generation_.fetch_add(1, std::memory_order_relaxed);
        stop_requested_ = true;

        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_custom_name_ = display_name;
            pending_custom_url_ = url;
            pending_custom_valid_ = true;
            pending_station_index_ = -1;
            pending_navigation_steps_ = 0;
        }
        music_playback_state_.store(1, std::memory_order_relaxed);
        audio_focus_blocked_.store(false, std::memory_order_relaxed);

        PostCommand(Command::PLAY_CUSTOM_URL);
        // Prepare can reconfigure audio and Wi-Fi, so run it after releasing
        // submission_mutex_. Playback's own focus claim waits on focus_lock.
        // A later Stop may enter the submission lock meanwhile; its final
        // release waits for this Prepare and therefore wins the focus state.
    }
    Application::GetInstance().PrepareExternalAudioPlayback();
    return result;
}

void RadioService::PostCommand(Command command, bool user_control) {
    // All user-control callers hold submission_mutex_. Focus notifications
    // originate on the application task and never supersede user input.
    if (user_control && command != Command::PLAY_PAUSE) pending_toggle_parity_ = false;
    CommandMessage message{command, user_control ? command_sequence_.fetch_add(1, std::memory_order_relaxed) + 1 : 0};
    auto queue = static_cast<QueueHandle_t>(queue_);
    if (!queue) {
        return;
    }
    if (xQueueSend(queue, &message, 0) == pdTRUE) {
        return;
    }
    // Audio focus is already an atomic state; a saturated queue does not need
    // another redundant notification. Never clear all queued user controls.
    if (command == Command::FOCUS_CHANGED) return;
    CommandMessage discarded;
    const bool dropped = xQueueReceive(queue, &discarded, 0) == pdTRUE;
    if (xQueueSendToFront(queue, &message, 0) == pdTRUE) {
        if (dropped)
            ESP_LOGW(TAG, "radio queue full; prioritized command=%d over oldest=%d",
                     static_cast<int>(command), static_cast<int>(discarded.command));
        return;
    }
    // A focus notification may refill the slot between receive and send.
    // Keep the newest user command in a task-consumed fallback mailbox.
    deferred_command_ = message;
    deferred_command_valid_ = true;
    ESP_LOGW(TAG, "radio queue full; command deferred=%d sequence=%lu",
             static_cast<int>(command), static_cast<unsigned long>(message.sequence));
}

void RadioService::ReleaseExternalAudioIfNotReplacing() {
    // Focus operations are serialized with new URL/station submissions, but
    // submission_mutex_ is not held while Application reconfigures audio.
    std::lock_guard<std::mutex> focus_lock(audio_focus_mutex_);
    {
        std::lock_guard<std::mutex> submission_lock(submission_mutex_);
        if (replacement_pending_.load(std::memory_order_acquire)) {
            ESP_LOGI(TAG, "retaining external audio for pending stream replacement");
            return;
        }
    }
    Application::GetInstance().SetExternalAudioActive(false);
}

void RadioService::ReleaseExternalAudioIfFocusBlocked() {
    // Recheck after taking the focus lock: an already queued focus event must
    // not turn off a newer song whose Prepare() has since restored playback.
    std::lock_guard<std::mutex> focus_lock(audio_focus_mutex_);
    if (!audio_focus_blocked_.load(std::memory_order_relaxed)) return;
    {
        std::lock_guard<std::mutex> submission_lock(submission_mutex_);
        if (replacement_pending_.load(std::memory_order_acquire)) return;
    }
    Application::GetInstance().SetExternalAudioActive(false);
}

void RadioService::SetExternalAudioActiveSerialized(bool active) {
    std::lock_guard<std::mutex> focus_lock(audio_focus_mutex_);
    Application::GetInstance().SetExternalAudioActive(active);
}

void RadioService::PublishCurrentStation() {
    if (station_index_ < 0 || station_index_ >= StationCount()) return;
    const auto& station = kStations[station_index_];
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        published_station_name_ = station.name;
        published_station_codec_ = station.codec;
        published_station_bitrate_ = station.bitrate_kbps;
        published_station_index_.store(station_index_, std::memory_order_release);
    }
}

void RadioService::ApplyPendingControl() {
    Command pending;
    CommandMessage deferred{Command::FOCUS_CHANGED, 0};
    bool has_pending = false;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (pending_custom_valid_) {
            pending = Command::PLAY_CUSTOM_URL;
            has_pending = true;
        } else if (pending_station_index_ >= 0) {
            pending = Command::SELECT_STATION;
            has_pending = true;
        } else if (pending_navigation_steps_ > 0) {
            pending = Command::NEXT;
            has_pending = true;
        } else if (pending_navigation_steps_ < 0) {
            pending = Command::PREV;
            has_pending = true;
        }
    }
    // A saturated command queue may have dropped the wakeup. The mailbox is
    // still owned until HandleCommand consumes it on this player task.
    if (has_pending) HandleCommand(pending);
    {
        std::lock_guard<std::mutex> submission_lock(submission_mutex_);
        if (deferred_command_valid_) {
            deferred = deferred_command_;
            deferred_command_valid_ = false;
        }
    }
    if (deferred.sequence != 0) HandleCommand(deferred.command, deferred.sequence);
}

void RadioService::WaitForRetry(int delay_ms, uint32_t stream_generation) {
    TickType_t remaining = pdMS_TO_TICKS(delay_ms);
    auto queue = static_cast<QueueHandle_t>(queue_);
    while (remaining > 0 && play_requested_.load(std::memory_order_relaxed) &&
           !stop_requested_.load(std::memory_order_relaxed) &&
           !audio_focus_blocked_.load(std::memory_order_relaxed) &&
           stream_generation == stream_generation_.load(std::memory_order_relaxed)) {
        const TickType_t step = std::min(remaining, pdMS_TO_TICKS(200));
        const TickType_t before = xTaskGetTickCount();
        CommandMessage message;
        if (queue && xQueueReceive(queue, &message, step) == pdTRUE) {
            HandleCommand(message.command, message.sequence);
            ApplyPendingControl();
        } else if (!queue) {
            vTaskDelay(step);
        }
        const TickType_t elapsed = xTaskGetTickCount() - before;
        remaining = elapsed >= remaining ? 0 : remaining - elapsed;
    }
}

void RadioService::Task() {
    while (true) {
        CommandMessage message;
        auto queue = static_cast<QueueHandle_t>(queue_);
        if (queue && xQueueReceive(queue, &message, pdMS_TO_TICKS(250)) == pdTRUE) {
            HandleCommand(message.command, message.sequence);
        }
        ApplyPendingControl();
        ReapFallbackReaders();

        // An external caller publishes the replacement payload after it
        // invalidates the old stream. Do not reopen the old station or release
        // its audio focus during that short handoff window.
        if (replacement_pending_.load(std::memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (!play_requested_) {
            reconnect_attempt_ = 0;
            if (playback_release_pending_.exchange(false, std::memory_order_relaxed)) {
                NotifyPlaybackReleased();
            }
            continue;
        }
        if (audio_focus_blocked_.load(std::memory_order_relaxed)) {
            if (!focus_pause_logged_) {
                ESP_LOGI(TAG, "radio paused by audio focus");
                SetUi("Paused", "XiaoZhi is using audio");
                focus_pause_logged_ = true;
            }
            ReleaseExternalAudioIfFocusBlocked();
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        focus_pause_logged_ = false;
        if (!WifiManager::GetInstance().IsConnected()) {
            SetUi("Waiting WiFi", "Need network");
            WaitForRetry(1000, stream_generation_.load(std::memory_order_relaxed));
            continue;
        }

        const uint32_t generation = stream_generation_.load(std::memory_order_relaxed);
        PlayCurrentStation(generation);
        if (generation != stream_generation_.load(std::memory_order_relaxed)) {
            reconnect_attempt_ = 0;
            continue;
        }
        if (replacement_pending_.load(std::memory_order_acquire)) continue;
        if (skip_reconnect_once_) {
            skip_reconnect_once_ = false;
            reconnect_attempt_ = 0;
            continue;
        }
        if (play_requested_ && playing_custom_url_) {
            if (custom_url_stream_completed_) {
                if (FinishCustomUrlIfCurrent(generation, true, "Music ended"))
                    ReleaseExternalAudioIfNotReplacing();
                continue;
            }
            if (!custom_url_fatal_error_ && reconnect_attempt_ < kCustomUrlMaxReconnectAttempts) {
                ++reconnect_attempt_;
                int delay_ms = std::min(4000, 800 + reconnect_attempt_ * 800);
                ESP_LOGW(TAG, "music url reconnect scheduled title=%s attempt=%d delay=%dms",
                         kStations[station_index_].name.c_str(), reconnect_attempt_, delay_ms);
                SetUi("Reconnecting", "Music network retry");
                WaitForRetry(delay_ms, generation);
                continue;
            }
            if (FinishCustomUrlIfCurrent(
                    generation, false,
                    custom_url_fatal_error_ ? "Music unavailable" : "Music interrupted"))
                ReleaseExternalAudioIfNotReplacing();
            continue;
        }
        if (play_requested_) {
            reconnect_attempt_++;
            int delay_ms = std::min(5000, 500 + reconnect_attempt_ * 500);
            ESP_LOGW(TAG, "radio reconnect scheduled station=%s attempt=%d delay=%dms",
                     kStations[station_index_].name.c_str(), reconnect_attempt_, delay_ms);
            if (reconnect_attempt_ >= 5) {
                SetUi("Error", "Multiple failures");
            } else {
                SetUi("Reconnecting", "Stream ended");
            }
            // Still claiming playback: do not hand the mic/I2S back between retries.
            WaitForRetry(delay_ms, generation);
        }
    }
}

void RadioService::HandleCommand(Command command, uint32_t sequence) {
    // Serialize consumption of the URL/station mailbox with submission of a
    // newer replacement. This also prevents an old queued toggle from
    // undoing the most recent Stop or Pause.
    std::unique_lock<std::mutex> submission_lock(submission_mutex_);
    if (sequence != 0 && sequence != command_sequence_.load(std::memory_order_relaxed)) {
        ESP_LOGI(TAG, "ignored stale radio command=%d sequence=%lu latest=%lu",
                 static_cast<int>(command), static_cast<unsigned long>(sequence),
                 static_cast<unsigned long>(command_sequence_.load(std::memory_order_relaxed)));
        return;
    }
    if (replacement_pending_.load(std::memory_order_acquire) &&
        (command == Command::PLAY_PAUSE || command == Command::PAUSE ||
         command == Command::STOP || command == Command::NEXT || command == Command::PREV)) {
        ESP_LOGI(TAG, "ignored superseded radio command=%d during stream replacement",
                 static_cast<int>(command));
        return;
    }
    const int count = StationCount();
    if (count <= 0) {
        ESP_LOGW(TAG, "radio no stations available");
        return;
    }
    
    if (station_index_ < 0 || station_index_ >= count) {
        station_index_ = 0;
    }

    const char* ui_state = nullptr;
    const char* ui_detail = nullptr;
    bool release_external = false;
    bool focus_release = false;
    bool save_station = false;
    
    switch (command) {
        case Command::PLAY_PAUSE: {
            const bool toggle = pending_toggle_parity_;
            pending_toggle_parity_ = false;
            if (!toggle) {
                ESP_LOGI(TAG, "coalesced even number of radio play/pause taps");
                break;
            }
            music_playback_state_.store(0, std::memory_order_relaxed);
            play_requested_ = !play_requested_.load(std::memory_order_relaxed);
            stop_requested_ = !play_requested_.load(std::memory_order_relaxed);
            reconnect_attempt_ = 0;
            const bool playing = play_requested_.load(std::memory_order_relaxed);
            if (!playing) {
                release_external = true;
                if (playing_custom_url_) {
                    playing_custom_url_ = false;
                    if (last_radio_station_index_ >= 0 && last_radio_station_index_ < StationCount()) {
                        station_index_ = last_radio_station_index_;
                    }
                }
            }
            ui_state = playing ? "Connecting" : "Paused";
            ui_detail = playing ? "Opening stream" : "Stopped";
            ESP_LOGI(TAG, "radio %s station=%s", playing ? "play requested" : "paused", kStations[station_index_].name.c_str());
            break;
        }
        case Command::PAUSE:
            music_playback_state_.store(0, std::memory_order_relaxed);
            play_requested_ = false;
            stop_requested_ = true;
            reconnect_attempt_ = 0;
            release_external = true;
            ui_state = "Paused";
            ui_detail = "Music paused";
            break;
        case Command::STOP:
            music_playback_state_.store(0, std::memory_order_relaxed);
            play_requested_ = false;
            stop_requested_ = true;
            reconnect_attempt_ = 0;
            if (playing_custom_url_) {
                playing_custom_url_ = false;
                if (last_radio_station_index_ >= 0 && last_radio_station_index_ < StationCount()) {
                    station_index_ = last_radio_station_index_;
                }
            }
            release_external = true;
            ui_state = "Stopped";
            ui_detail = "Ready";
            ESP_LOGI(TAG, "radio stopped");
            break;
        case Command::NEXT: {
            int steps;
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                steps = std::max(0, pending_navigation_steps_);
                if (steps > 0) pending_navigation_steps_ = 0;
            }
            if (steps == 0) break;
            music_playback_state_.store(0, std::memory_order_relaxed);
            if (playing_custom_url_ || station_index_ == custom_station_index_) {
                play_requested_ = false;
                stop_requested_ = true;
                reconnect_attempt_ = 0;
                release_external = true;
                ui_state = "Stopped";
                ui_detail = "Ask XiaoZhi for next song";
                ESP_LOGI(TAG, "music next ignored until a fresh URL is provided");
                break;
            }
            for (int i = 0; i < steps; ++i) NextStation(1);
            save_station = true;
            play_requested_ = true;
            stop_requested_ = false;
            reconnect_attempt_ = 0;
            ui_state = "Connecting";
            ui_detail = "Next station";
            ESP_LOGI(TAG, "radio next station=%s", kStations[station_index_].name.c_str());
            break;
        }
        case Command::PREV: {
            int steps;
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                steps = std::max(0, -pending_navigation_steps_);
                if (steps > 0) pending_navigation_steps_ = 0;
            }
            if (steps == 0) break;
            music_playback_state_.store(0, std::memory_order_relaxed);
            if (playing_custom_url_) {
                playing_custom_url_ = false;
                if (last_radio_station_index_ >= 0 && last_radio_station_index_ < StationCount()) {
                    station_index_ = last_radio_station_index_;
                }
            }
            for (int i = 0; i < steps; ++i) NextStation(-1);
            save_station = true;
            play_requested_ = true;
            stop_requested_ = false;
            reconnect_attempt_ = 0;
            ui_state = "Connecting";
            ui_detail = "Previous station";
            ESP_LOGI(TAG, "radio previous station=%s", kStations[station_index_].name.c_str());
            break;
        }
        case Command::SELECT_STATION: {
            int requested;
            int category_filter;
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                requested = pending_station_index_;
                category_filter = pending_category_filter_;
                pending_station_index_ = -1;
            }
            if (requested < 0 || requested >= catalog_station_count_) {
                if (requested >= 0)
                    ESP_LOGW(TAG, "selected station request out of range index=%d count=%d",
                             requested, catalog_station_count_);
                break;
            }
            music_playback_state_.store(0, std::memory_order_relaxed);
            playing_custom_url_ = false;
            station_index_ = requested;
            save_station = true;
            active_category_filter_ = category_filter;
            play_requested_ = true;
            stop_requested_ = false;
            reconnect_attempt_ = 0;
            skip_reconnect_once_ = false;
            replacement_pending_.store(false, std::memory_order_release);
            ui_state = "Connecting";
            ui_detail = "Selected from directory";
            ESP_LOGI(TAG, "radio directory selected station=%s category=%d",
                     kStations[station_index_].name.c_str(), active_category_filter_);
            break;
        }
        case Command::FOCUS_CHANGED:
            if (audio_focus_blocked_.load(std::memory_order_relaxed)) {
                focus_release = true;
                ui_state = "Paused";
                ui_detail = "XiaoZhi is using audio";
            } else if (play_requested_) {
                ui_state = "Connecting";
                ui_detail = "Audio focus restored";
                ESP_LOGI(TAG, "radio audio focus restored, resume station=%s", kStations[station_index_].name.c_str());
            }
            break;
        case Command::PLAY_CUSTOM_URL: {
            std::string name;
            std::string url;
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                if (!pending_custom_valid_) break;
                name = std::move(pending_custom_name_);
                url = std::move(pending_custom_url_);
                pending_custom_valid_ = false;
            }
            if (!playing_custom_url_ && station_index_ >= 0 &&
                station_index_ < catalog_station_count_)
                last_radio_station_index_ = station_index_;
            auto& station = kStations[custom_station_index_];
            station.name = std::move(name);
            station.urls[0] = std::move(url);
            station.urls[1].clear();
            station.urls[2].clear();
            station.codec = "MP3";
            station.bitrate_kbps = 0;
            station_index_ = custom_station_index_;
            custom_url_speaking_grace_until_.store(
                xTaskGetTickCount() + kCustomUrlSpeakingGraceTicks, std::memory_order_relaxed);
            playing_custom_url_ = true;
            music_playback_state_.store(1, std::memory_order_relaxed);
            play_requested_ = true;
            stop_requested_ = false;
            reconnect_attempt_ = 0;
            replacement_pending_.store(false, std::memory_order_release);
            ui_state = "Connecting";
            ui_detail = "Opening music URL";
            ESP_LOGI(TAG, "music url play requested title=%s", kStations[station_index_].name.c_str());
            break;
        }
    }
    const uint32_t action_sequence = command_sequence_.load(std::memory_order_relaxed);
    submission_lock.unlock();
    // UI, NVS and audio reconfiguration may wait for other tasks. Never hold
    // the submission lock across them; a new song can invalidate this action.
    PublishCurrentStation();
    if (save_station) SaveStationIndex();
    if (release_external) ReleaseExternalAudioIfNotReplacing();
    if (focus_release) ReleaseExternalAudioIfFocusBlocked();
    if (ui_state && action_sequence == command_sequence_.load(std::memory_order_relaxed))
        SetUi(ui_state, ui_detail);
}

void RadioService::PlayCurrentStation(uint32_t stream_generation) {
    const int count = StationCount();
    if (count <= 0 || station_index_ < 0 || station_index_ >= count) {
        SetUi("Error", "No station");
        return;
    }

    // Hold external audio across all URL fallbacks so the duplex I2S is not
    // reconfigured (mic on/TX off) between attempts.
    SetExternalAudioActiveSerialized(true);

    const auto station_urls = kStations[station_index_].urls;
    bool tried_any = false;
    int attempted_sources = 0;
    int permanent_failures = 0;
    stop_requested_ = false;
    int url_order[3] = {0, 1, 2};
    
    if (station_index_ < static_cast<int>(last_success_url_.size()) && 
        last_success_url_[station_index_] > 0 && last_success_url_[station_index_] < 3) {
        url_order[0] = last_success_url_[station_index_];
        url_order[1] = 0;
        url_order[2] = last_success_url_[station_index_] == 1 ? 2 : 1;
    }
    
    for (int order = 0; order < 3 && play_requested_ && !stop_requested_ &&
         stream_generation == stream_generation_.load(std::memory_order_relaxed); ++order) {
        if (audio_focus_blocked_.load(std::memory_order_relaxed)) {
            ESP_LOGI(TAG, "radio station open deferred by audio focus");
            return;
        }
        const int i = url_order[order];
        if (i < 0 || i >= 3) continue;
        const std::string url = station_urls[i];
        if (url.empty()) {
            continue;
        }
        tried_any = true;
        attempted_sources++;
        if (PlayUrl(url, i, stream_generation)) {
            return;
        }
        if (last_url_permanent_error_) {
            permanent_failures++;
        }
        if (stream_generation != stream_generation_.load(std::memory_order_relaxed)) {
            return;
        }
        SetUi("Connecting", "Trying fallback");
        WaitForRetry(300, stream_generation);
    }
    // A command handled during the final fallback wait may have selected a
    // different station. Do not skip or report an error for that new request.
    if (stream_generation != stream_generation_.load(std::memory_order_relaxed)) return;
    if (!playing_custom_url_ && play_requested_ && attempted_sources > 0 &&
        permanent_failures == attempted_sources) {
        const std::string failed_station = kStations[station_index_].name;
        NextStation(1);
        PublishCurrentStation();
        SaveStationIndex();
        skip_reconnect_once_ = true;
        SetUi("Connecting", "Skipped unavailable station");
        ESP_LOGW(TAG, "radio station permanently unavailable; skipped station=%s next=%s sources=%d",
                 failed_station.c_str(), kStations[station_index_].name.c_str(), attempted_sources);
        return;
    }
    if (play_requested_ && !stop_requested_) {
        SetUi("Error", tried_any ? "All sources failed" : "No source");
    }
    if (!play_requested_.load(std::memory_order_relaxed) ||
        stop_requested_.load(std::memory_order_relaxed))
        ReleaseExternalAudioIfNotReplacing();
}

bool RadioService::PlayUrl(const std::string& url, int url_index, uint32_t stream_generation) {
    ReapFallbackReaders();
    if (g_deferred_reader_count.load() >= g_deferred_readers.size()) {
        ESP_LOGE(TAG, "too many blocked stream readers; waiting for cleanup");
        SetUi("Reconnecting", "Network cleanup pending");
        return false;
    }
    const std::string station_name = kStations[station_index_].name;
    const int64_t t_start_us = esp_timer_get_time();
    SetUi("Connecting", url_index == 0 ? "Opening stream" : "Fallback source");
    ResetAudioLeveler();
    last_url_permanent_error_ = false;
    if (playing_custom_url_) {
        custom_url_stream_completed_ = false;
        custom_url_fatal_error_ = false;
    }

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    // TLS handshake to qtfm/qingting can take several seconds on slower WAN
    // links (e.g. phone hotspots). Keep connect/read timeout generous; mid-stream
    // hangs are still cut by the 5s stall watchdog after data starts flowing.
    config.timeout_ms = playing_custom_url_ ? 10000 : 8000;
    // esp_http_client keeps this buffer in scarce internal SRAM.  Compressed
    // stream headroom lives in the 48 KB PSRAM buffer below, so a larger HTTP
    // scratch buffer only increases the risk of TLS/MQTT allocation failures.
    config.buffer_size = 4096;
    config.buffer_size_tx = 1024;
    config.disable_auto_redirect = false;
    config.max_redirection_count = 5;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        SetUi("Error", "HTTP init failed");
        return false;
    }

    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0 ESP32 Radio");
    esp_http_client_set_header(client, "Accept", "audio/mpeg,*/*");
    esp_http_client_set_header(client, "Icy-MetaData", "0");
    // Live streams through qtfm/qingting drop the connection after a few
    // minutes; close each response so reconnect starts clean.
    esp_http_client_set_header(client, "Connection", "close");
    if (IsNetEaseMusicUrl(url)) {
        esp_http_client_set_header(client, "User-Agent",
                                   "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                                   "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36");
        esp_http_client_set_header(client, "Accept", "audio/mpeg,audio/*;q=0.9,*/*;q=0.8");
        esp_http_client_set_header(client, "Accept-Language", "zh-CN,zh;q=0.9,en;q=0.8");
        esp_http_client_set_header(client, "Referer", "https://music.163.com/");
        esp_http_client_set_header(client, "Origin", "https://music.163.com");
        esp_http_client_set_header(client, "Connection", "close");
    }

    esp_err_t err = esp_http_client_open(client, 0);
    ESP_LOGI(TAG, "timing: http open %s in %d ms", err == ESP_OK ? "ok" : "FAILED",
             static_cast<int>((esp_timer_get_time() - t_start_us) / 1000));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open failed station=%s url_index=%d err=%s url=%s", station_name.c_str(), url_index, esp_err_to_name(err), url.c_str());
        esp_http_client_cleanup(client);
        return false;
    }

    if (!g_stream_stall_timer) {
        esp_timer_create_args_t stall_args = {
            .callback = [](void*) {
                std::lock_guard<std::mutex> lock(g_stream_client_mutex);
                auto* client = g_active_stream_client.load(std::memory_order_relaxed);
                const int64_t last = g_last_stream_progress_us.load(std::memory_order_relaxed);
                if (!client || !last) return;
                const int64_t age = esp_timer_get_time() - last;
                if (age > kReadStallTimeoutUs) {
                    ESP_LOGE(TAG, "stream read stalled for %lld ms; shutting down socket",
                             static_cast<long long>(age / 1000));
                    g_last_stream_progress_us.store(0, std::memory_order_relaxed);
                    // Only shutdown the TCP socket so the blocked read returns.
                    // Never esp_http_client_close here: the radio task still owns
                    // the handle and a cross-thread close causes a store fault.
                    const int sock = esp_http_client_get_socket(client);
                    if (sock >= 0) {
                        ::shutdown(sock, SHUT_RDWR);
                    }
                }
            },
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "radio_stall",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&stall_args, &g_stream_stall_timer) == ESP_OK) {
            esp_timer_start_periodic(g_stream_stall_timer, 1000000);
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_stream_client_mutex);
        g_active_stream_client.store(client, std::memory_order_relaxed);
        g_last_stream_progress_us.store(esp_timer_get_time(), std::memory_order_relaxed);
    }

    auto release_client = [client]() {
        // Detach under the timer lock, then close outside it. No callback can
        // dereference this handle after DetachStreamClient returns.
        DetachStreamClient(client);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    };

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (stream_generation != stream_generation_.load(std::memory_order_relaxed)) {
        ESP_LOGI(TAG, "discard stale stream response station=%s url_index=%d", station_name.c_str(), url_index);
        release_client();
        return false;
    }
    if (status < 200 || status >= 400) {
        ESP_LOGW(TAG, "bad stream status station=%s url_index=%d status=%d len=%d url=%s",
                 station_name.c_str(), url_index, status, content_length, url.c_str());
        last_url_permanent_error_ = status == 400 || status == 401 || status == 403 ||
                                    status == 404 || status == 410;
        if (playing_custom_url_ && last_url_permanent_error_) {
            ESP_LOGW(TAG, "custom music url rejected status=%d, stopping retries station=%s",
                     status, station_name.c_str());
            custom_url_fatal_error_ = true;
            play_requested_ = false;
            stop_requested_ = true;
            playback_release_pending_.store(true, std::memory_order_relaxed);
            ReleaseExternalAudioIfNotReplacing();
            SetUi("Error", status == 403 ? "Music URL rejected" : "Music URL unavailable");
        }
        release_client();
        return false;
    }

    if (playing_custom_url_ && content_length > 0 && content_length < 256 * 1024) {
        ESP_LOGW(TAG, "music url rejected as short preview len=%d station=%s", content_length,
                 station_name.c_str());
        custom_url_fatal_error_ = true;
        play_requested_ = false;
        stop_requested_ = true;
        playback_release_pending_.store(true, std::memory_order_relaxed);
        ReleaseExternalAudioIfNotReplacing();
        SetUi("Error", "Need full song URL");
        release_client();
        return false;
    }

    ESP_LOGI(TAG, "stream open station=%s url_index=%d status=%d len=%d url=%s",
             station_name.c_str(), url_index, status, content_length, url.c_str());
    if (playing_custom_url_) {
        ESP_LOGI(TAG, "music url open ok len=%d url=%s", content_length, url.c_str());
    }
    ESP_LOGI(TAG, "timing: headers received %d ms after connect start",
             static_cast<int>((esp_timer_get_time() - t_start_us) / 1000));
    SetUi("Buffering", "Filling buffer");

    HMP3Decoder decoder = MP3InitDecoder();
    auto* read_buffer = static_cast<uint8_t*>(heap_caps_malloc(kReadBufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* pcm_buffer = static_cast<int16_t*>(heap_caps_malloc(kPcmMaxSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* mono_buffer = static_cast<int16_t*>(heap_caps_malloc(kPcmMaxSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* output_buffer = static_cast<int16_t*>(heap_caps_malloc(kPcmOutputMaxSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    // Pointers must be unique heap blocks: a smashed/double free here asserts
    // ("free() target pointer is outside heap areas") and reboots the device.
    auto safe_free = [](auto*& p) {
        if (p) {
            heap_caps_free(p);
            p = nullptr;
        }
    };
    if (!decoder || !read_buffer || !pcm_buffer || !mono_buffer || !output_buffer) {
        ESP_LOGE(TAG, "decoder alloc failed");
        SetUi("Error", "No decoder memory");
        if (decoder) {
            MP3FreeDecoder(decoder);
            decoder = nullptr;
        }
        safe_free(read_buffer);
        safe_free(pcm_buffer);
        safe_free(mono_buffer);
        safe_free(output_buffer);
        release_client();
        return false;
    }

    // Start the background reader (see StreamReaderTask).
    auto* reader = new (std::nothrow) StreamReader();
    if (reader) {
        reader->client = client;
        reader->custom_url = playing_custom_url_;
        reader->content_length = content_length;
        reader->empty_limit = playing_custom_url_ ? kCustomUrlEmptyReadLimit : kRadioEmptyReadLimit;
        reader->station_name = station_name;
        reader->url_index = url_index;
        reader->done = xSemaphoreCreateBinary();
        const size_t* sizes = playing_custom_url_ ? kMusicRingBytes : kRadioRingBytes;
        const size_t count = playing_custom_url_ ? std::size(kMusicRingBytes) : std::size(kRadioRingBytes);
        for (size_t i = 0; i < count && !reader->ring; ++i) {
            reader->ring = xStreamBufferCreateWithCaps(sizes[i], 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (reader->ring)
                ESP_LOGI(TAG, "stream ring %u KB psram_free=%u psram_largest=%u internal_free=%u",
                         static_cast<unsigned>(sizes[i] / 1024),
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        }
    }
    bool reader_running = false;
    TaskHandle_t reader_task = nullptr;
    if (reader && reader->done && reader->ring) {
        reader_running = xTaskCreatePinnedToCoreWithCaps(StreamReaderTask, "radio_reader", 6144, reader, 5,
                                                         &reader_task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
    }
    auto stop_reader = [&]() -> bool {
        // Returns true when the reader no longer touches `client`.
        if (!reader)
            return true;
        const int64_t stop_start_us = esp_timer_get_time();
        if (reader_running) {
            reader->stop.store(true);
            if (xSemaphoreTake(reader->done, pdMS_TO_TICKS(300)) != pdTRUE) {
                // Blocked in a socket read: unblock it like the stall watchdog does.
                {
                    std::lock_guard<std::mutex> lock(g_stream_client_mutex);
                    const int sock = esp_http_client_get_socket(client);
                    if (sock >= 0)
                        ::shutdown(sock, SHUT_RDWR);
                }
                if (xSemaphoreTake(reader->done, pdMS_TO_TICKS(1200)) != pdTRUE) {
                    // Do not close/free a client still in esp_http_client_read.
                    // Transfer all reader resources to the radio task's
                    // deferred list and continue the requested station change.
                    DetachStreamClient(client);
                    if (DeferStreamReader({reader, reader_task, client})) {
                        ESP_LOGW(
                            TAG,
                            "stream reader still blocked after %lld ms; deferred cleanup count=%u",
                            static_cast<long long>((esp_timer_get_time() - stop_start_us) / 1000),
                            static_cast<unsigned>(g_deferred_reader_count.load()));
                    } else {
                        ESP_LOGE(TAG,
                                 "stream reader still blocked; deferred list full, leaking it");
                    }
                    reader = nullptr;
                    return false;
                }
            }
            reader_running = false;
        }
        DestroyStreamReader(reader, reader_task);
        reader = nullptr;
        const int64_t elapsed_ms = (esp_timer_get_time() - stop_start_us) / 1000;
        if (elapsed_ms >= 200)
            ESP_LOGI(TAG, "stream reader stopped in %lld ms", static_cast<long long>(elapsed_ms));
        return true;
    };
    if (!reader_running) {
        ESP_LOGE(TAG, "stream reader start failed");
        SetUi("Error", "No stream memory");
        stop_reader();
        MP3FreeDecoder(decoder);
        decoder = nullptr;
        safe_free(read_buffer);
        safe_free(pcm_buffer);
        safe_free(mono_buffer);
        safe_free(output_buffer);
        release_client();
        return false;
    }

    uint8_t* read_ptr = read_buffer;
    playback_clock_.Begin(url, stream_generation);
    int bytes_left = 0;
    int decoded_frames = 0;
    bool logged_complete = false;
    int decode_errors = 0;
    size_t total_bytes = 0;
    bool stream_failed = false;
    bool stream_completed = false;
    SetExternalAudioActiveSerialized(true);
    // Reopen TX only when output is missing. Reopening on every URL while the
    // mic is still active parks I2S in "Pending out channel" and mutes us.
    if (auto* tab5_codec = static_cast<Tab5AudioCodec*>(Board::GetInstance().GetAudioCodec())) {
        if (!Board::GetInstance().GetAudioCodec()->output_enabled()) {
            tab5_codec->ReopenOutput();
        }
    }

    while (play_requested_ && !stop_requested_ && WifiManager::GetInstance().IsConnected() &&
           stream_generation == stream_generation_.load(std::memory_order_relaxed)) {
        CommandMessage message;
        auto queue = static_cast<QueueHandle_t>(queue_);
        while (queue && xQueueReceive(queue, &message, 0) == pdTRUE) {
            HandleCommand(message.command, message.sequence);
        }
        ApplyPendingControl();
        if (!play_requested_ || stop_requested_ ||
            stream_generation != stream_generation_.load(std::memory_order_relaxed)) {
            break;
        }
        if (ShouldYieldAudio()) {
            SetUi("Paused", "XiaoZhi is using audio");
            ESP_LOGI(TAG, "radio yielded audio focus station=%s", station_name.c_str());
            break;
        }

        if (read_ptr != read_buffer && bytes_left > 0) {
            memmove(read_buffer, read_ptr, bytes_left);
            read_ptr = read_buffer;
        }
        // Only use the smaller target before the first decoded frame.  The old
        // `|| playing_custom_url_` condition accidentally kept every music URL
        // at a 2 KB target for the entire song, turning normal network jitter
        // directly into audible drop-outs.
        const int steady_target = playing_custom_url_ ? kMusicReadTargetBytes : kRadioReadTargetBytes;
        // On-demand songs start after 6 KB (about 0.4 s of 128 kbps audio); live radio keeps
        // the larger first fill because its servers deliver in bursts.
        const int initial_target = playing_custom_url_ ? 6 * 1024 : kInitialReadTargetBytes;
        const int target_bytes = decoded_frames == 0 ? initial_target : steady_target;
        while (bytes_left < target_bytes && bytes_left < kReadBufferSize &&
               play_requested_ && !stop_requested_ &&
               stream_generation == stream_generation_.load(std::memory_order_relaxed)) {
            const size_t got = xStreamBufferReceive(
                reader->ring, read_buffer + bytes_left, kReadBufferSize - bytes_left,
                pdMS_TO_TICKS(20));
            if (got > 0) {
                bytes_left += static_cast<int>(got);
                continue;
            }
            const int reader_state = reader->state.load();
            if (reader_state != 0 && xStreamBufferIsEmpty(reader->ring)) {
                if (reader_state == 1) {
                    if (!logged_complete) {
                        logged_complete = true;
                        ESP_LOGI(TAG, "stream completed station=%s url_index=%d frames=%d buffered=%d",
                                 station_name.c_str(), url_index, decoded_frames, bytes_left);
                    }
                    if (bytes_left <= 0)
                        stream_completed = true;
                } else if (bytes_left <= 0) {
                    if (playing_custom_url_ && decoded_frames > 0)
                        SetUi("Reconnecting", "Music network stall");
                    stream_failed = true;
                }
                break;
            }
            // Ring momentarily empty: decode what we have rather than wait for a full target.
            if (decoded_frames > 0 && bytes_left >= 2048)
                break;
            if (ShouldYieldAudio())
                break;
        }
        total_bytes = reader->total.load();
        if (stream_generation != stream_generation_.load(std::memory_order_relaxed)) {
            break;
        }
        if (stream_completed) {
            if (playing_custom_url_) {
                FinishCustomUrlIfCurrent(stream_generation, true, "Music ended");
            }
            break;
        }
        if (stream_failed) {
            SetUi(playing_custom_url_ ? "Reconnecting" : "Reconnecting",
                  playing_custom_url_ ? "Music network retry" : "Read failed");
            break;
        }

        if (bytes_left <= 0) {
            SetUi("Buffering", "Waiting for data");
            vTaskDelay(pdMS_TO_TICKS(80));
            continue;
        }

        int offset = MP3FindSyncWord(read_ptr, bytes_left);
        if (offset < 0) {
            ESP_LOGW(TAG, "mp3 sync not found station=%s url_index=%d bytes=%d", station_name.c_str(), url_index, bytes_left);
            bytes_left = 0;
            read_ptr = read_buffer;
            SetUi("Buffering", "Finding sync");
            continue;
        }
        read_ptr += offset;
        bytes_left -= offset;

        int mp3_err = MP3Decode(decoder, &read_ptr, &bytes_left, pcm_buffer, 0);
        if (mp3_err == ERR_MP3_INDATA_UNDERFLOW || mp3_err == ERR_MP3_MAINDATA_UNDERFLOW) {
            // A trailing partial frame of a finished download can never decode.
            if (reader->state.load() == 1 && xStreamBufferIsEmpty(reader->ring) &&
                bytes_left < 2048) {
                bytes_left = 0;
                read_ptr = read_buffer;
            }
            continue;
        }
        if (mp3_err != ERR_MP3_NONE) {
            ++decode_errors;
            ESP_LOGW(TAG, "mp3 decode failed station=%s url_index=%d err=%d count=%d", station_name.c_str(), url_index, mp3_err, decode_errors);
            // Sustained INVALID_FRAMEHEADER on a live stream usually means the
            // decoder state is already desynced. Recycle it before internal
            // pointers go bad and FreeBuffers asserts.
            if (decode_errors == 4) {
                ESP_LOGW(TAG, "recycling mp3 decoder after repeated errors");
                MP3FreeDecoder(decoder);
                decoder = MP3InitDecoder();
                if (!decoder) {
                    ESP_LOGE(TAG, "decoder reinit failed");
                    break;
                }
            }
            if (decode_errors > 8) {
                SetUi(playing_custom_url_ ? "Reconnecting" : "Reconnecting",
                      playing_custom_url_ ? "Music decode retry" : "Decode errors");
                stream_failed = true;
                break;
            }
            if (bytes_left > 0) {
                ++read_ptr;
                --bytes_left;
            }
            continue;
        }

        decode_errors = 0;
        MP3FrameInfo frame_info = {};
        MP3GetLastFrameInfo(decoder, &frame_info);
        WritePcm(pcm_buffer, frame_info.outputSamps, frame_info.nChans, frame_info.samprate,
                 mono_buffer, kPcmMaxSamples, output_buffer, kPcmOutputMaxSamples,
                 stream_generation);
        if (decoded_frames == 0) {
            if (station_index_ >= 0 && station_index_ < static_cast<int>(last_success_url_.size())) {
                last_success_url_[station_index_] = url_index;
            }
            if (!playing_custom_url_) {
                reconnect_attempt_ = 0;
            }
            ESP_LOGI(TAG, "radio remembered successful url station=%s url_index=%d", station_name.c_str(), url_index);
        }
        ++decoded_frames;
        if (decoded_frames == 1 || decoded_frames % 64 == 0) {
            char detail[64];
            snprintf(detail, sizeof(detail), "%d kbps %d Hz %u KB",
                     frame_info.bitrate / 1000, frame_info.samprate, static_cast<unsigned>(total_bytes / 1024));
            SetUi("Playing", detail);
            if (decoded_frames == 1) {
                ESP_LOGI(TAG, "timing: first audio frame %d ms after connect start",
                         static_cast<int>((esp_timer_get_time() - t_start_us) / 1000));
            }
            ESP_LOGI(TAG, "playing station=%s frames=%d rate=%d channels=%d total=%u KB",
                     station_name.c_str(), decoded_frames, frame_info.samprate, frame_info.nChans,
                     static_cast<unsigned>(total_bytes / 1024));
        }
    }

    const bool reader_stopped = stop_reader();
    if (decoder) {
        MP3FreeDecoder(decoder);
        decoder = nullptr;
    }
    safe_free(read_buffer);
    safe_free(pcm_buffer);
    safe_free(mono_buffer);
    safe_free(output_buffer);
    if (reader_stopped) {
        release_client();
    }
    // Keep external audio claimed while PlayCurrentStation may still try the
    // next URL. Releasing here re-enables the mic and reconfigures the shared
    // full-duplex I2S, which can leave TX disabled ("channel has not been
    // enabled yet") so later frames log "playing" with no sound.
    if (!play_requested_.load(std::memory_order_relaxed) ||
        stop_requested_.load(std::memory_order_relaxed))
        ReleaseExternalAudioIfNotReplacing();
    return decoded_frames > 0 && play_requested_ && !stop_requested_ && !stream_failed && !stream_completed &&
           stream_generation == stream_generation_.load(std::memory_order_relaxed) &&
           !audio_focus_blocked_.load(std::memory_order_relaxed);
}

bool RadioService::IsXiaozhiAudioState() const {
    auto app_state = Application::GetInstance().GetDeviceState();
    return app_state != kDeviceStateIdle;
}

bool RadioService::IsCustomUrlSpeakingGraceActive() const {
    if (!playing_custom_url_ || !play_requested_.load(std::memory_order_relaxed)) {
        return false;
    }
    return static_cast<int32_t>(custom_url_speaking_grace_until_ - xTaskGetTickCount()) > 0;
}

bool RadioService::IsAutonomousCustomUrlSpeaking(int previous_state, int current_state) const {
    if (!playing_custom_url_ || !play_requested_.load(std::memory_order_relaxed) ||
        current_state != kDeviceStateSpeaking) {
        return false;
    }
    return previous_state != kDeviceStateListening &&
           previous_state != kDeviceStateConnecting &&
           previous_state != kDeviceStateAudioTesting;
}

bool RadioService::ShouldYieldAudio() const {
    auto app_state = Application::GetInstance().GetDeviceState();
    if (app_state == kDeviceStateSpeaking && IsCustomUrlSpeakingGraceActive()) {
        return false;
    }
    if (app_state == kDeviceStateSpeaking &&
        playing_custom_url_ &&
        play_requested_.load(std::memory_order_relaxed) &&
        !audio_focus_blocked_.load(std::memory_order_relaxed)) {
        return false;
    }
    return audio_focus_blocked_.load(std::memory_order_relaxed) ||
           app_state != kDeviceStateIdle;
}

void RadioService::OnDeviceStateChanged(int previous_state, int current_state) {
    (void)previous_state;
    bool blocked = current_state != kDeviceStateIdle;
    if (current_state == kDeviceStateSpeaking && blocked &&
        (IsCustomUrlSpeakingGraceActive() || IsAutonomousCustomUrlSpeaking(previous_state, current_state))) {
        ESP_LOGI(TAG, "audio focus speaking ignored during music url playback previous=%d", previous_state);
        blocked = false;
        Application::GetInstance().Schedule([]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateSpeaking) {
                app.AbortSpeaking(kAbortReasonNone);
                app.SetDeviceState(kDeviceStateIdle);
            }
        });
    }
    const bool was_blocked = audio_focus_blocked_.exchange(blocked, std::memory_order_relaxed);
    if (blocked != was_blocked) {
        ESP_LOGI(TAG, "audio focus %s by XiaoZhi state=%d play_requested=%d",
                 blocked ? "blocked" : "released", current_state, play_requested_.load(std::memory_order_relaxed));
        PostCommand(Command::FOCUS_CHANGED, false);
    }
}

int RadioService::AdjacentCatalogIndex(int from, int delta, int category_filter) const {
    const int count = catalog_station_count_;
    if (count <= 0 || from < 0 || from >= count) return from;
    int candidate = from;
    for (int attempt = 0; attempt < count; ++attempt) {
        candidate = (candidate + delta + count) % count;
        if (category_filter < 0 ||
            static_cast<int>(kStations[candidate].category) == category_filter)
            return candidate;
    }
    return from;
}

void RadioService::NextStation(int delta) {
    const int count = catalog_station_count_;
    if (count <= 0) {
        ESP_LOGW(TAG, "NextStation: no stations available");
        return;
    }
    if (station_index_ < 0 || station_index_ >= count) {
        station_index_ = 0;
        ESP_LOGW(TAG, "NextStation: station_index_ was out of range, reset to 0");
    }
    if (active_category_filter_ >= 0) {
        int candidate = station_index_;
        bool found = false;
        for (int attempt = 0; attempt < count; ++attempt) {
            candidate = (candidate + delta + count) % count;
            if (static_cast<int>(kStations[candidate].category) == active_category_filter_) {
                station_index_ = candidate;
                found = true;
                break;
            }
        }
        if (!found) {
            ESP_LOGW(TAG, "no stations in active category=%d; keeping current station", active_category_filter_);
            return;
        }
    } else {
        station_index_ = (station_index_ + delta + count) % count;
    }
    ESP_LOGI(TAG, "NextStation: new index=%d count=%d name=%s", station_index_, count, kStations[station_index_].name.c_str());
}

bool RadioService::FinishCustomUrlIfCurrent(uint32_t stream_generation, bool completed,
                                            const char* detail) {
    // Every replacement and user control advances stream_generation_ while
    // holding submission_mutex_. Keep terminal state and its UI event in that
    // same critical section so an old stream cannot mark a newer song ended
    // or unavailable after a replacement has been submitted.
    std::lock_guard<std::mutex> submission_lock(submission_mutex_);
    if (stream_generation != stream_generation_.load(std::memory_order_relaxed) ||
        replacement_pending_.load(std::memory_order_acquire) || !play_requested_ ||
        stop_requested_ || !playing_custom_url_)
        return false;

    custom_url_stream_completed_ = completed;
    music_playback_state_.store(completed ? 2 : 3, std::memory_order_relaxed);
    play_requested_ = false;
    stop_requested_ = true;
    playback_release_pending_.store(true, std::memory_order_relaxed);
    reconnect_attempt_ = 0;
    SetUi("Stopped", detail);
    return true;
}

void RadioService::SetUi(const char* state, const char* detail) {
    if (!desktop_ui_ && !state_callback_) return;
    const auto& station = kStations[station_index_];
    char meta[96];
    snprintf(meta, sizeof(meta), "%s %d kbps  %s", station.codec.c_str(), station.bitrate_kbps, detail ? detail : "");
    if (state_callback_) {
        state_callback_(station.name.c_str(), state, meta);
        return;
    }
#if !CONFIG_QDTECH_TAB5_NATIVE_UI
    if (lvgl_port_lock(100)) {
        desktop_ui_->SetRadioState(station.name.c_str(), state, meta);
        lvgl_port_unlock();
    }
#endif
}

void RadioService::WritePcm(const int16_t* pcm, int samples, int channels, int sample_rate,
                            int16_t* mono_buffer, int mono_capacity, int16_t* output_buffer,
                            int output_capacity, uint32_t stream_generation) {
    if (!pcm || !mono_buffer || !output_buffer || samples <= 0 || channels <= 0 || sample_rate <= 0) {
        return;
    }

    int frames = channels == 2 ? samples / 2 : samples;
    if (frames <= 0 || frames > mono_capacity) {
        ESP_LOGW(TAG, "pcm frame capacity exceeded frames=%d capacity=%d", frames, mono_capacity);
        return;
    }

    if (channels == 2) {
        for (int i = 0; i < frames; ++i) {
            mono_buffer[i] = Clamp16(((int)pcm[i * 2] + (int)pcm[i * 2 + 1]) / 2);
        }
    } else {
        memcpy(mono_buffer, pcm, frames * sizeof(int16_t));
    }

    auto* codec = Board::GetInstance().GetAudioCodec();
    const int out_rate = codec->output_sample_rate();
    int out_frames = frames;
    if (sample_rate == out_rate) {
        memcpy(output_buffer, mono_buffer, frames * sizeof(int16_t));
    } else {
        out_frames = std::max(1, frames * out_rate / sample_rate);
        if (out_frames > output_capacity) {
            ESP_LOGW(TAG, "pcm output capacity exceeded frames=%d capacity=%d", out_frames, output_capacity);
            return;
        }
        for (int i = 0; i < out_frames; ++i) {
            int64_t src_pos_q16 = (int64_t)i * sample_rate * 65536 / out_rate;
            int src_index = src_pos_q16 >> 16;
            int frac = src_pos_q16 & 0xffff;
            if (src_index >= frames - 1) {
                output_buffer[i] = mono_buffer[frames - 1];
            } else {
                int a = mono_buffer[src_index];
                int b = mono_buffer[src_index + 1];
                output_buffer[i] = Clamp16((a * (65536 - frac) + b * frac) >> 16);
            }
        }
    }

    ApplyAudioLeveler(output_buffer, out_frames);
    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
    }
    codec->OutputData(output_buffer, out_frames);
    playback_clock_.Advance(out_frames, out_rate, stream_generation);
    // Radio bypasses AudioOutputTask; keep the power manager from treating
    // the speaker as idle and closing it mid-stream.
    Application::GetInstance().GetAudioService().NoteOutputActivity();
}

void RadioService::ResetAudioLeveler() {
    audio_gain_q12_ = 4096;
}

void RadioService::ApplyAudioLeveler(int16_t* pcm, int samples) {
    if (!pcm || samples <= 0) {
        return;
    }

    int peak = 0;
    int64_t abs_sum = 0;
    for (int i = 0; i < samples; ++i) {
        const int16_t sample = pcm[i];
        int v = std::abs(static_cast<int>(sample));
        peak = std::max(peak, v);
        abs_sum += v;
    }

    const int avg_abs = static_cast<int>(abs_sum / static_cast<int64_t>(samples));
    audio_level_.store(std::clamp(avg_abs / 90, 0, 100), std::memory_order_relaxed);
    if (avg_abs <= 0 || peak <= 0) {
        return;
    }

    constexpr int kTargetAvg = 5000;
    constexpr int kLimiterPeak = 24500;
    constexpr int32_t kMinGainQ12 = 2300;   // about 0.56x
    constexpr int32_t kMaxGainQ12 = 8200;   // about 2.00x

    int32_t desired = static_cast<int32_t>((static_cast<int64_t>(kTargetAvg) * 4096) / avg_abs);
    const int32_t peak_limited = static_cast<int32_t>((static_cast<int64_t>(kLimiterPeak) * 4096) / peak);
    desired = std::min(desired, peak_limited);
    desired = std::clamp(desired, kMinGainQ12, kMaxGainQ12);

    if (desired < audio_gain_q12_) {
        audio_gain_q12_ = (audio_gain_q12_ * 3 + desired) / 4;
    } else {
        audio_gain_q12_ = (audio_gain_q12_ * 31 + desired) / 32;
    }

    for (int i = 0; i < samples; ++i) {
        int32_t v = static_cast<int32_t>((static_cast<int64_t>(pcm[i]) * audio_gain_q12_) >> 12);
        int32_t av = std::abs(v);
        if (av > 23500) {
            int32_t over = av - 23500;
            av = 23500 + over / 4;
            if (av > 29500) {
                av = 29500;
            }
            v = v < 0 ? -av : av;
        }
        pcm[i] = Clamp16(v);
    }
}
