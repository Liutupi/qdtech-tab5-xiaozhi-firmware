#include "tab5_ota.h"

#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "application.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/idf_additions.h"
#include "psa/crypto.h"
#include "tab5_sd.h"
#include "wifi_manager.h"

static const char* TAG = "Tab5Ota";

static constexpr char kLatestReleaseUrl[] =
    "https://api.github.com/repos/Liutupi/qdtech-tab5-xiaozhi-firmware/releases/latest";
// Release asset naming: qdtech-tab5-<tag>-app.bin, listed in SHA256SUMS.txt.
static constexpr char kAssetPrefix[] = "qdtech-tab5-";
static constexpr char kAssetSuffix[] = "-app.bin";
static constexpr char kSumsAsset[] = "SHA256SUMS.txt";
static constexpr char kUpdaterProject[] = "tab5_updater";
static constexpr size_t kMaxJson = 65536;
static constexpr size_t kWriteBuffer = 32 * 1024;
static constexpr size_t kMinImageSize = 256 * 1024;
static constexpr char kSdRoot[] = "/sdcard";
static constexpr char kOtaDir[] = "/sdcard/ota";
static constexpr char kPartPath[] = "/sdcard/ota/tab5-update.part";
static constexpr char kBinPath[] = "/sdcard/ota/tab5-update.bin";
static constexpr char kMetaPath[] = "/sdcard/ota/tab5-update.txt";
static constexpr char kMetaTmpPath[] = "/sdcard/ota/tab5-update.tmp";

namespace {

std::string StripLeadingV(const std::string& version) {
    if (!version.empty() && (version[0] == 'v' || version[0] == 'V')) return version.substr(1);
    return version;
}

std::vector<int> ParseVersionParts(const std::string& text) {
    std::vector<int> parts;
    int value = 0;
    bool in_number = false;
    for (char ch : text) {
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            value = value * 10 + (ch - '0');
            in_number = true;
        } else {
            if (in_number) {
                parts.push_back(value);
                value = 0;
                in_number = false;
            }
            if (ch == '-' || ch == '+') break;
        }
    }
    if (in_number) parts.push_back(value);
    return parts;
}

bool IsNewerVersion(const std::string& current, const std::string& latest) {
    const auto a = ParseVersionParts(StripLeadingV(current));
    const auto b = ParseVersionParts(StripLeadingV(latest));
    const size_t count = std::max(a.size(), b.size());
    for (size_t i = 0; i < count; ++i) {
        const int x = i < a.size() ? a[i] : 0;
        const int y = i < b.size() ? b[i] : 0;
        if (y > x) return true;
        if (y < x) return false;
    }
    return false;
}

bool IsSha256Hex(const std::string& text) {
    if (text.size() != 64) return false;
    for (char ch : text) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
    }
    return true;
}

std::string ToLower(std::string text) {
    for (auto& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}

std::string ToHex(const uint8_t* digest, size_t length) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0x0f]);
    }
    return out;
}

std::string FormatMegabytes(size_t bytes) {
    char text[24];
    std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

// ---- small HTTP text fetch (release JSON, SHA256SUMS.txt) ----
struct TextBuffer {
    std::string data;
    size_t max_bytes = kMaxJson;
    bool truncated = false;
};

esp_err_t TextEvent(esp_http_client_event_t* evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->user_data || !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }
    if (esp_http_client_get_status_code(evt->client) != 200) return ESP_OK;  // skip redirect bodies
    auto* buffer = static_cast<TextBuffer*>(evt->user_data);
    const size_t room = buffer->max_bytes > buffer->data.size() ? buffer->max_bytes - buffer->data.size() : 0;
    const size_t copy = std::min(room, static_cast<size_t>(evt->data_len));
    buffer->data.append(static_cast<const char*>(evt->data), copy);
    if (copy < static_cast<size_t>(evt->data_len)) buffer->truncated = true;
    return ESP_OK;
}

bool FetchText(const char* url, size_t max_bytes, std::string* output) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        TextBuffer buffer;
        buffer.max_bytes = max_bytes;

        esp_http_client_config_t config = {};
        config.url = url;
        config.timeout_ms = 15000;
        config.event_handler = TextEvent;
        config.user_data = &buffer;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.buffer_size = 4096;
        config.buffer_size_tx = 4096;
        config.max_redirection_count = 6;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) return false;
        const esp_app_desc_t* desc = esp_app_get_description();
        const std::string agent = std::string("qdtech-tab5/") + (desc ? desc->version : "0");
        esp_http_client_set_header(client, "User-Agent", agent.c_str());
        esp_http_client_set_header(client, "Accept", "application/vnd.github+json,text/plain,*/*");
        const esp_err_t err = esp_http_client_perform(client);
        const int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
        if (err == ESP_OK && status == 200 && !buffer.data.empty() && !buffer.truncated) {
            *output = std::move(buffer.data);
            return true;
        }
        ESP_LOGW(TAG, "fetch %s attempt %d failed err=%s status=%d", url, attempt + 1,
                 esp_err_to_name(err), status);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

// ---- firmware download straight to the SD card ----
struct DownloadContext {
    FILE* file = nullptr;
    uint8_t* buffer = nullptr;
    size_t fill = 0;
    size_t received = 0;
    size_t expected = 0;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    bool failed = false;
    int last_percent = -1;
    int64_t last_report_us = 0;
    std::function<void(int)> on_progress;
};

esp_err_t DownloadEvent(esp_http_client_event_t* evt) {
    auto* ctx = static_cast<DownloadContext*>(evt->user_data);
    if (evt->event_id != HTTP_EVENT_ON_DATA || !ctx || ctx->failed || !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }
    if (esp_http_client_get_status_code(evt->client) != 200) return ESP_OK;  // redirect bodies

    const uint8_t* p = static_cast<const uint8_t*>(evt->data);
    size_t n = static_cast<size_t>(evt->data_len);
    if (psa_hash_update(&ctx->hash, p, n) != PSA_SUCCESS) {
        ctx->failed = true;
        return ESP_OK;
    }
    ctx->received += n;
    while (n > 0) {
        const size_t take = std::min(kWriteBuffer - ctx->fill, n);
        std::memcpy(ctx->buffer + ctx->fill, p, take);
        ctx->fill += take;
        p += take;
        n -= take;
        if (ctx->fill == kWriteBuffer) {
            if (std::fwrite(ctx->buffer, 1, ctx->fill, ctx->file) != ctx->fill) {
                ESP_LOGE(TAG, "SD write failed errno=%d", errno);
                ctx->failed = true;
                return ESP_OK;
            }
            ctx->fill = 0;
        }
    }
    if (ctx->expected > 0 && ctx->on_progress) {
        const int percent = static_cast<int>(static_cast<uint64_t>(ctx->received) * 100 / ctx->expected);
        const int64_t now = esp_timer_get_time();
        if (percent != ctx->last_percent && now - ctx->last_report_us >= 400000) {
            ctx->last_percent = percent;
            ctx->last_report_us = now;
            ctx->on_progress(std::min(percent, 100));
        }
    }
    return ESP_OK;
}

// Confirms the downloaded file is a Tab5 (ESP32-P4) firmware image for `version`.
bool CheckImageFile(const char* path, const std::string& version, std::string* problem) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        *problem = "无法读取安装包";
        return false;
    }
    uint8_t head[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)];
    const size_t n = std::fread(head, 1, sizeof(head), f);
    std::fclose(f);
    if (n != sizeof(head)) {
        *problem = "安装包不完整";
        return false;
    }
    esp_image_header_t header;
    std::memcpy(&header, head, sizeof(header));
    esp_app_desc_t desc;
    std::memcpy(&desc, head + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(desc));
    if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.chip_id != ESP_CHIP_ID_ESP32P4 ||
        desc.magic_word != ESP_APP_DESC_MAGIC_WORD || std::strncmp(desc.project_name, "xiaozhi", 7) != 0) {
        *problem = "安装包不是 Tab5 固件";
        return false;
    }
    if (std::strncmp(desc.version, version.c_str(), version.size()) != 0) {
        ESP_LOGE(TAG, "image version %.32s does not match release %s", desc.version, version.c_str());
        *problem = "安装包版本不符";
        return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------

Tab5Ota& Tab5Ota::GetInstance() {
    static Tab5Ota instance;
    return instance;
}

std::string Tab5Ota::CurrentVersion() const {
    const esp_app_desc_t* desc = esp_app_get_description();
    return desc ? desc->version : "";
}

void Tab5Ota::SetStatusCallback(StatusCallback callback) {
    Status snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(callback);
        if (status_.text.empty()) {
            status_.text = "当前版本 v" + StripLeadingV(CurrentVersion());
            status_.button = "检查更新";
        }
        snapshot = status_;
    }
    if (callback_) callback_(snapshot);
}

Tab5Ota::Status Tab5Ota::GetStatus() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.text.empty()) {
        status_.text = "当前版本 v" + StripLeadingV(CurrentVersion());
        status_.button = "检查更新";
    }
    return status_;
}

void Tab5Ota::SetStatus(const std::string& text, const std::string& button, int progress, bool busy) {
    Status snapshot;
    StatusCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.text = text;
        status_.button = button;
        status_.progress = progress;
        status_.busy = busy;
        snapshot = status_;
        callback = callback_;
    }
    if (callback) callback(snapshot);
}

void Tab5Ota::HandleButton() {
    // Runs on the application task (internal stack), which is where flash APIs are safe.
    if (busy_.load(std::memory_order_acquire)) return;

    bool install = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        install = update_ready_;
    }

    if (install) {
        const esp_partition_t* factory =
            esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
        const esp_partition_t* updater =
            esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
        esp_app_desc_t desc = {};
        if (!factory || !updater || esp_ota_get_partition_description(updater, &desc) != ESP_OK ||
            std::strncmp(desc.project_name, kUpdaterProject, std::strlen(kUpdaterProject)) != 0) {
            ESP_LOGE(TAG, "updater partition missing or invalid");
            SetStatus("缺少升级程序，请用 USB 刷入完整固件", "检查更新", -1, false);
            return;
        }
        size_t asset_size = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            asset_size = release_.asset_size;
        }
        if (asset_size > factory->size) {
            SetStatus("安装包过大，请用 USB 刷入", "检查更新", -1, false);
            return;
        }
    }

    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

    static TaskHandle_t worker = nullptr;
    static std::atomic<int> pending_action{-1};
    pending_action.store(install ? 1 : 0, std::memory_order_release);
    if (worker == nullptr) {
        auto entry = [](void*) {
            while (true) {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                const int action = pending_action.exchange(-1, std::memory_order_acq_rel);
                if (action == 0) {
                    Tab5Ota::GetInstance().Run(Action::Check);
                } else if (action == 1) {
                    Tab5Ota::GetInstance().Run(Action::Install);
                }
            }
        };
        BaseType_t ok = xTaskCreateWithCaps(entry, "tab5_ota", 14336, nullptr, 2, &worker,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ok != pdPASS) {
            worker = nullptr;
            ok = xTaskCreate(entry, "tab5_ota", 14336, nullptr, 2, &worker);
        }
        if (ok != pdPASS) {
            worker = nullptr;
            busy_.store(false, std::memory_order_release);
            SetStatus("内存不足，请稍后重试", "检查更新", -1, false);
            return;
        }
    }
    xTaskNotifyGive(worker);
}

void Tab5Ota::Run(Action action) {
    if (action == Action::Check) {
        Check();
    } else {
        Install();
    }
    busy_.store(false, std::memory_order_release);
}

bool Tab5Ota::FetchLatestRelease(ReleaseInfo* release) {
    std::string json;
    if (!FetchText(kLatestReleaseUrl, kMaxJson, &json)) return false;

    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) {
        ESP_LOGE(TAG, "release JSON parse failed");
        return false;
    }
    cJSON* tag = cJSON_GetObjectItem(root, "tag_name");
    if (!cJSON_IsString(tag) || !tag->valuestring || !tag->valuestring[0]) {
        cJSON_Delete(root);
        return false;
    }
    *release = ReleaseInfo{};
    release->tag = tag->valuestring;
    release->version = StripLeadingV(release->tag);

    const std::string expected_asset = std::string(kAssetPrefix) + release->tag + kAssetSuffix;
    std::string sums_url;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(root, "assets")) {
        cJSON* name = cJSON_GetObjectItem(item, "name");
        cJSON* url = cJSON_GetObjectItem(item, "browser_download_url");
        if (!cJSON_IsString(name) || !cJSON_IsString(url)) continue;
        const std::string asset = name->valuestring;
        if (asset == kSumsAsset) {
            sums_url = url->valuestring;
        } else if (asset == expected_asset) {
            cJSON* size = cJSON_GetObjectItem(item, "size");
            release->asset_name = asset;
            release->asset_url = url->valuestring;
            release->asset_size = cJSON_IsNumber(size) && size->valuedouble > 0
                                      ? static_cast<size_t>(size->valuedouble)
                                      : 0;
        }
    }
    cJSON_Delete(root);

    if (!release->asset_url.empty() && !sums_url.empty()) {
        std::string sums;
        if (FetchText(sums_url.c_str(), 8192, &sums)) {
            size_t start = 0;
            while (start < sums.size()) {
                size_t end = sums.find('\n', start);
                if (end == std::string::npos) end = sums.size();
                const std::string line = sums.substr(start, end - start);
                start = end + 1;
                if (line.size() >= 64 && line.find(release->asset_name) != std::string::npos) {
                    const std::string candidate = ToLower(line.substr(0, 64));
                    if (IsSha256Hex(candidate)) {
                        release->sha256 = candidate;
                        break;
                    }
                }
            }
        }
    }
    return true;
}

void Tab5Ota::Check() {
    const std::string current = CurrentVersion();
    if (!WifiManager::GetInstance().IsConnected()) {
        SetStatus("需要先连接网络", "检查更新", -1, false);
        return;
    }
    SetStatus("正在检查更新…", "请稍候", -1, true);

    ReleaseInfo release;
    if (!FetchLatestRelease(&release)) {
        SetStatus("检查失败，请稍后重试", "检查更新", -1, false);
        return;
    }
    if (!IsNewerVersion(current, release.version)) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            update_ready_ = false;
        }
        SetStatus("已是最新版本 v" + StripLeadingV(current), "检查更新", -1, false);
        return;
    }
    if (release.asset_url.empty() || release.sha256.size() != 64) {
        SetStatus("新版本 v" + release.version + " 暂无可用安装包", "检查更新", -1, false);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        release_ = release;
        update_ready_ = true;
    }
    SetStatus("发现新版本 v" + release.version + "（" + FormatMegabytes(release.asset_size) + "）",
              "立即升级", -1, false);
}

void Tab5Ota::Install() {
    ReleaseInfo release;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        release = release_;
    }
    auto fail = [this](const std::string& text) {
        ESP_LOGE(TAG, "install aborted: %s", text.c_str());
        SetStatus(text, "检查更新", -1, false);
        std::lock_guard<std::mutex> lock(mutex_);
        update_ready_ = false;
    };

    if (!WifiManager::GetInstance().IsConnected()) return fail("需要先连接网络");
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        return fail("请先结束当前对话再升级");
    }
    if (!Tab5SdReady() && !Tab5SdMountAndRegisterFs()) return fail("未检测到 SD 卡，升级需要 SD 卡");

    if (mkdir(kOtaDir, 0775) != 0 && errno != EEXIST) return fail("无法在 SD 卡创建升级目录");
    unlink(kPartPath);
    unlink(kBinPath);
    unlink(kMetaPath);
    unlink(kMetaTmpPath);

    uint64_t total_bytes = 0, free_bytes = 0;
    if (esp_vfs_fat_info(kSdRoot, &total_bytes, &free_bytes) == ESP_OK &&
        free_bytes < release.asset_size + 4u * 1024 * 1024) {
        return fail("SD 卡空间不足");
    }

    SetStatus("正在下载 v" + release.version + " …", "下载中", 0, true);
    if (!Download(release, kPartPath)) {
        unlink(kPartPath);
        return;  // Download() already reported the reason
    }

    SetStatus("正在校验安装包…", "校验中", 100, true);
    std::string problem;
    if (!CheckImageFile(kPartPath, release.version, &problem)) {
        unlink(kPartPath);
        return fail(problem);
    }
    if (rename(kPartPath, kBinPath) != 0) return fail("无法保存安装包");

    char meta[160];
    std::snprintf(meta, sizeof(meta), "sha256=%s\nsize=%u\nversion=%s\n", release.sha256.c_str(),
                  static_cast<unsigned>(release.asset_size), release.version.c_str());
    FILE* mf = std::fopen(kMetaTmpPath, "w");
    if (!mf || std::fputs(meta, mf) < 0 || std::fflush(mf) != 0) {
        if (mf) std::fclose(mf);
        unlink(kBinPath);
        return fail("无法写入升级信息");
    }
    std::fclose(mf);
    if (rename(kMetaTmpPath, kMetaPath) != 0) {
        unlink(kBinPath);
        return fail("无法保存升级信息");
    }

    SetStatus("下载完成，即将重启升级（屏幕将黑屏约一分钟，请勿断电）", "请勿断电", 100, true);
    vTaskDelay(pdMS_TO_TICKS(4000));

    // Flash APIs must run on the application task (this worker's stack may be in PSRAM).
    Application::GetInstance().Schedule([this]() {
        const esp_partition_t* updater =
            esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
        const esp_err_t err = updater ? esp_ota_set_boot_partition(updater) : ESP_ERR_NOT_FOUND;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "cannot select updater: %s", esp_err_to_name(err));
            unlink(kBinPath);
            unlink(kMetaPath);
            SetStatus("无法进入升级程序，已取消", "检查更新", -1, false);
            std::lock_guard<std::mutex> lock(mutex_);
            update_ready_ = false;
            return;
        }
        ESP_LOGI(TAG, "rebooting into updater");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    });
    // Keep the "busy" state until the restart happens.
    vTaskDelay(pdMS_TO_TICKS(15000));
}

bool Tab5Ota::Download(const ReleaseInfo& release, const char* path) {
    for (int attempt = 1; attempt <= 2; ++attempt) {
        DownloadContext ctx;
        ctx.expected = release.asset_size;
        ctx.buffer = static_cast<uint8_t*>(heap_caps_malloc(kWriteBuffer, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        ctx.file = std::fopen(path, "wb");
        if (!ctx.buffer || !ctx.file) {
            if (ctx.buffer) heap_caps_free(ctx.buffer);
            if (ctx.file) std::fclose(ctx.file);
            SetStatus("无法写入 SD 卡", "检查更新", -1, false);
            return false;
        }
        if (psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&ctx.hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
            heap_caps_free(ctx.buffer);
            std::fclose(ctx.file);
            SetStatus("校验模块初始化失败", "检查更新", -1, false);
            return false;
        }
        ctx.on_progress = [this, &release](int percent) {
            SetStatus("正在下载 v" + release.version + " … " + std::to_string(percent) + "%", "下载中",
                      percent, true);
        };

        esp_http_client_config_t config = {};
        config.url = release.asset_url.c_str();
        config.timeout_ms = 30000;
        config.event_handler = DownloadEvent;
        config.user_data = &ctx;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.buffer_size = 4096;
        config.buffer_size_tx = 4096;
        config.max_redirection_count = 8;
        esp_http_client_handle_t client = esp_http_client_init(&config);
        esp_err_t err = ESP_FAIL;
        int status = 0;
        if (client) {
            const esp_app_desc_t* desc = esp_app_get_description();
            const std::string agent = std::string("qdtech-tab5/") + (desc ? desc->version : "0");
            esp_http_client_set_header(client, "User-Agent", agent.c_str());
            err = esp_http_client_perform(client);
            status = esp_http_client_get_status_code(client);
            esp_http_client_cleanup(client);
        }

        bool written = !ctx.failed;
        if (written && ctx.fill > 0) {
            written = std::fwrite(ctx.buffer, 1, ctx.fill, ctx.file) == ctx.fill;
        }
        written = written && std::fflush(ctx.file) == 0;
        std::fclose(ctx.file);
        heap_caps_free(ctx.buffer);

        uint8_t digest[32];
        size_t digest_len = 0;
        const bool hashed = psa_hash_finish(&ctx.hash, digest, sizeof(digest), &digest_len) == PSA_SUCCESS &&
                            digest_len == sizeof(digest);
        if (!hashed) psa_hash_abort(&ctx.hash);

        const bool complete = err == ESP_OK && status == 200 && written &&
                              (release.asset_size == 0 || ctx.received == release.asset_size) &&
                              ctx.received >= kMinImageSize;
        if (complete && hashed) {
            if (ToHex(digest, sizeof(digest)) == release.sha256) {
                SetStatus("下载完成", "下载中", 100, true);
                return true;
            }
            ESP_LOGE(TAG, "SHA-256 mismatch (attempt %d)", attempt);
            SetStatus("安装包校验失败，已取消", "检查更新", -1, false);
            return false;  // never retry a hash mismatch silently
        }
        ESP_LOGW(TAG, "download attempt %d failed err=%s status=%d received=%u expected=%u", attempt,
                 esp_err_to_name(err), status, static_cast<unsigned>(ctx.received),
                 static_cast<unsigned>(release.asset_size));
        unlink(path);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    SetStatus("下载失败，请检查网络后重试", "立即升级", -1, false);
    return false;
}
