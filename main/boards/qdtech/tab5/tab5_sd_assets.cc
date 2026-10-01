#include "tab5_sd_assets.h"

#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "psa/crypto.h"

namespace tab5_sd_assets {
namespace {

const char* TAG = "Tab5SdAssets";
constexpr const char* kDir = "/sdcard/tab5";
constexpr size_t kWriteBuffer = 16 * 1024;

std::string Hex(const uint8_t* digest, size_t length) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        out.push_back(digits[digest[i] >> 4]);
        out.push_back(digits[digest[i] & 15]);
    }
    return out;
}

struct Download {
    FILE* file = nullptr;
    uint8_t* buffer = nullptr;
    size_t fill = 0;
    size_t received = 0;
    size_t limit = 0;
    bool failed = false;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
};

esp_err_t OnEvent(esp_http_client_event_t* evt) {
    auto* d = static_cast<Download*>(evt->user_data);
    if (evt->event_id != HTTP_EVENT_ON_DATA || !d || d->failed || !evt->data || evt->data_len <= 0)
        return ESP_OK;
    if (esp_http_client_get_status_code(evt->client) != 200)
        return ESP_OK;  // redirect bodies
    const uint8_t* p = static_cast<const uint8_t*>(evt->data);
    size_t n = static_cast<size_t>(evt->data_len);
    d->received += n;
    if (d->received > d->limit || psa_hash_update(&d->hash, p, n) != PSA_SUCCESS) {
        d->failed = true;
        return ESP_OK;
    }
    while (n > 0) {
        const size_t take = std::min(kWriteBuffer - d->fill, n);
        std::memcpy(d->buffer + d->fill, p, take);
        d->fill += take;
        p += take;
        n -= take;
        if (d->fill == kWriteBuffer) {
            if (std::fwrite(d->buffer, 1, d->fill, d->file) != d->fill) {
                d->failed = true;
                return ESP_OK;
            }
            d->fill = 0;
        }
    }
    return ESP_OK;
}

bool FetchOnce(const Asset& asset, const std::string& url, const std::string& tmp) {
    Download d;
    d.limit = asset.size;
    d.buffer = static_cast<uint8_t*>(heap_caps_malloc(kWriteBuffer, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    d.file = std::fopen(tmp.c_str(), "wb");
    if (!d.buffer || !d.file || psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_setup(&d.hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        if (d.buffer)
            heap_caps_free(d.buffer);
        if (d.file)
            std::fclose(d.file);
        ESP_LOGE(TAG, "cannot prepare %s", tmp.c_str());
        return false;
    }
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = 30000;
    config.event_handler = OnEvent;
    config.user_data = &d;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 4096;
    config.buffer_size_tx = 2048;
    config.max_redirection_count = 8;
    esp_err_t err = ESP_FAIL;
    int status = 0;
    if (auto client = esp_http_client_init(&config)) {
        esp_http_client_set_header(client, "User-Agent", "qdtech-tab5");
        err = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
    }
    bool ok = !d.failed && (d.fill == 0 || std::fwrite(d.buffer, 1, d.fill, d.file) == d.fill);
    ok = std::fflush(d.file) == 0 && ok;
    std::fclose(d.file);
    heap_caps_free(d.buffer);
    uint8_t digest[32];
    size_t digest_len = 0;
    const bool hashed = psa_hash_finish(&d.hash, digest, sizeof(digest), &digest_len) == PSA_SUCCESS;
    if (!hashed)
        psa_hash_abort(&d.hash);
    const bool verified = ok && err == ESP_OK && status == 200 && d.received == asset.size && hashed &&
                          Hex(digest, digest_len) == asset.sha256;
    if (!verified) {
        ESP_LOGW(TAG, "%s: download failed err=%s http=%d got=%u/%u", asset.name, esp_err_to_name(err),
                 status, unsigned(d.received), unsigned(asset.size));
        unlink(tmp.c_str());
    }
    return verified;
}

}  // namespace

const Asset& TextFont() {
    static const Asset asset = {
        "font_noto_sans_common_30_4.bin",
        2609092,
        "b9690d5babd46d650aa06bbcae0acd36194fded289ce150453de8a421af9793d",
        {
            "https://ghfast.top/https://github.com/Liutupi/qdtech-tab5-xiaozhi-firmware/releases/"
            "download/sd-assets-v1/font_noto_sans_common_30_4.bin",
            "https://github.com/Liutupi/qdtech-tab5-xiaozhi-firmware/releases/download/sd-assets-v1/"
            "font_noto_sans_common_30_4.bin",
            "https://gh-proxy.com/https://github.com/Liutupi/qdtech-tab5-xiaozhi-firmware/releases/"
            "download/sd-assets-v1/font_noto_sans_common_30_4.bin",
        },
    };
    return asset;
}

std::string PathOf(const Asset& asset) {
    return std::string(kDir) + "/" + asset.name;
}

bool Present(const Asset& asset) {
    struct stat st = {};
    return stat(PathOf(asset).c_str(), &st) == 0 && size_t(st.st_size) == asset.size;
}

bool Ensure(const Asset& asset) {
    if (Present(asset))
        return true;
    mkdir(kDir, 0775);
    const std::string path = PathOf(asset);
    const std::string tmp = path + ".part";
    for (const auto& url : asset.urls) {
        ESP_LOGI(TAG, "%s: downloading (%u bytes)", asset.name, unsigned(asset.size));
        if (FetchOnce(asset, url, tmp)) {
            unlink(path.c_str());
            if (rename(tmp.c_str(), path.c_str()) == 0) {
                ESP_LOGI(TAG, "%s: ready on SD", asset.name);
                return true;
            }
            ESP_LOGE(TAG, "%s: rename failed errno=%d", asset.name, errno);
            unlink(tmp.c_str());
        }
    }
    return false;
}

void* LoadToPsram(const Asset& asset, size_t* size_out) {
    if (!Present(asset))
        return nullptr;
    FILE* f = std::fopen(PathOf(asset).c_str(), "rb");
    if (!f)
        return nullptr;
    auto* data = static_cast<uint8_t*>(heap_caps_malloc(asset.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    size_t got = 0;
    if (data) {
        while (got < asset.size) {
            const size_t n = std::fread(data + got, 1, std::min<size_t>(64 * 1024, asset.size - got), f);
            if (n == 0)
                break;
            got += n;
        }
    }
    std::fclose(f);
    if (!data || got != asset.size) {
        if (data)
            heap_caps_free(data);
        ESP_LOGE(TAG, "%s: read failed (%u/%u)", asset.name, unsigned(got), unsigned(asset.size));
        return nullptr;
    }
    if (size_out)
        *size_out = got;
    return data;
}

}  // namespace tab5_sd_assets
