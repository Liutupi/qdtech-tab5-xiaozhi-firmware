#include "photo_service.h"

#include "tab5_sd.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <jpeg_decoder.h>

#define TAG "PhotoService"

namespace {
constexpr int kDesktopWidth = 480;
constexpr int kDesktopHeight = 320;
constexpr const char* kPhotoDir = "/sdcard/photos";
constexpr const char* kPhotoDirs[] = {
    "/sdcard/photos",
    "/sdcard/PHOTOS",
    "/sdcard/Photos",
    "/sdcard/PHOTOS_READY",
    "/sdcard/photos_ready",
    "/sdcard",
};
constexpr size_t kMaxInputBytes = 4 * 1024 * 1024;
constexpr size_t kMaxOutputBytes = 960 * 640 * 2;
constexpr size_t kBackgroundPixels = kDesktopWidth * kDesktopHeight;
constexpr size_t kBackgroundBytes = kBackgroundPixels * 2;
constexpr TickType_t kSlideDelay = pdMS_TO_TICKS(6000);
constexpr TickType_t kIdleDelay = pdMS_TO_TICKS(500);

bool ends_with_photo_ext(const char* name) {
    if (!name) {
        return false;
    }
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower.ends_with(".jpg") || lower.ends_with(".jpeg");
}

bool is_hidden_or_resource_file(const char* name) {
    if (!name || name[0] == '\0') {
        return true;
    }
    return name[0] == '.' || (name[0] == '_' && name[1] == '.');
}

esp_jpeg_image_scale_t choose_scale(uint16_t width, uint16_t height) {
    if (width <= 960 && height <= 640 && static_cast<size_t>(width) * height * 2 <= kMaxOutputBytes) {
        return JPEG_IMAGE_SCALE_0;
    }
    if (width / 2 <= 960 && height / 2 <= 640 &&
        static_cast<size_t>(width / 2) * (height / 2) * 2 <= kMaxOutputBytes) {
        return JPEG_IMAGE_SCALE_1_2;
    }
    if (width / 4 <= 960 && height / 4 <= 640 &&
        static_cast<size_t>(width / 4) * (height / 4) * 2 <= kMaxOutputBytes) {
        return JPEG_IMAGE_SCALE_1_4;
    }
    return JPEG_IMAGE_SCALE_1_8;
}

uint16_t darken_rgb565(uint16_t color) {
    const uint16_t r = (color >> 11) & 0x1f;
    const uint16_t g = (color >> 5) & 0x3f;
    const uint16_t b = color & 0x1f;
    return static_cast<uint16_t>(((r * 3 / 5) << 11) | ((g * 3 / 5) << 5) | (b * 3 / 5));
}

uint16_t blur_sample_rgb565(const uint16_t* source, uint16_t width, uint16_t height, int x, int y) {
    uint32_t r = 0;
    uint32_t g = 0;
    uint32_t b = 0;
    uint32_t count = 0;
    for (int dy = -3; dy <= 3; dy += 2) {
        int sy = std::clamp(y + dy, 0, static_cast<int>(height) - 1);
        for (int dx = -3; dx <= 3; dx += 2) {
            int sx = std::clamp(x + dx, 0, static_cast<int>(width) - 1);
            const uint16_t color = source[sy * width + sx];
            r += (color >> 11) & 0x1f;
            g += (color >> 5) & 0x3f;
            b += color & 0x1f;
            ++count;
        }
    }
    return static_cast<uint16_t>(((r / count) << 11) | ((g / count) << 5) | (b / count));
}

void fill_portrait_background(const uint8_t* source_data, uint16_t source_width, uint16_t source_height,
                              uint8_t* background_data) {
    auto* background = reinterpret_cast<uint16_t*>(background_data);
    const auto* source = reinterpret_cast<const uint16_t*>(source_data);

    const int32_t scale_x = (kDesktopWidth * 256) / source_width;
    const int32_t scale_y = (kDesktopHeight * 256) / source_height;
    const int32_t scale = std::max(scale_x, scale_y);
    const int32_t scaled_width = (source_width * scale) / 256;
    const int32_t scaled_height = (source_height * scale) / 256;
    const int32_t x_offset = (scaled_width - kDesktopWidth) / 2;
    const int32_t y_offset = (scaled_height - kDesktopHeight) / 2;

    for (int y = 0; y < kDesktopHeight; ++y) {
        for (int x = 0; x < kDesktopWidth; ++x) {
            const int raw_x = static_cast<int>(((x + x_offset) * 256) / scale);
            const int raw_y = static_cast<int>(((y + y_offset) * 256) / scale);
            const int sx = std::clamp(raw_x, 0, static_cast<int>(source_width) - 1);
            const int sy = std::clamp(raw_y, 0, static_cast<int>(source_height) - 1);
            background[y * kDesktopWidth + x] = darken_rgb565(blur_sample_rgb565(source, source_width, source_height, sx, sy));
        }
    }
}
} // namespace

void PhotoService::Start(DesktopUI* desktop_ui) {
    desktop_ui_ = desktop_ui;
    if (desktop_ui_) {
        desktop_ui_->SetPhotoActiveCallback([this](bool active) { SetActive(active); });
        desktop_ui_->SetPhotoRefreshCallback([this]() { Refresh(); });
    }
    ESP_LOGI(TAG, "photo service registered");
}

void PhotoService::SetActive(bool active) {
    const bool previous = active_.exchange(active);
    if (previous != active) {
        ESP_LOGI(TAG, "photo playback %s", active ? "active" : "paused");
        if (active) {
            EnsureTaskStarted();
            refresh_requested_.store(true);
        }
    }
}

void PhotoService::Refresh() {
    EnsureTaskStarted();
    refresh_requested_.store(true);
    ESP_LOGI(TAG, "photo refresh requested");
}

void PhotoService::EnsureTaskStarted() {
    if (task_handle_) {
        return;
    }

    constexpr uint32_t stack_size = 6144;
    ESP_LOGI(TAG, "photo task create free_internal=%u largest_internal=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));

    BaseType_t ret = xTaskCreateWithCaps(
        TaskWrapper, "photo_service", stack_size, this, 2, &task_handle_,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret == pdPASS) {
        ESP_LOGI(TAG, "photo service task started stack=%u memory=psram",
                 static_cast<unsigned>(stack_size));
        return;
    }

    ESP_LOGW(TAG, "photo task PSRAM create failed ret=%ld, trying internal memory",
             static_cast<long>(ret));
    task_handle_ = nullptr;
    ret = xTaskCreate(TaskWrapper, "photo_service", stack_size, this, 2, &task_handle_);
    if (ret != pdPASS) {
        task_handle_ = nullptr;
        ESP_LOGE(TAG, "photo service task create failed ret=%ld free_internal=%u largest_internal=%u",
                 static_cast<long>(ret),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        SetUiState("Photos unavailable", "Not enough memory");
        return;
    }
    ESP_LOGI(TAG, "photo service task started stack=%u memory=internal",
             static_cast<unsigned>(stack_size));
}

void PhotoService::TaskWrapper(void* arg) {
    static_cast<PhotoService*>(arg)->TaskLoop();
}

void PhotoService::TaskLoop() {
    SetUiState("Photos", "Open /photos on SD card");

    while (true) {
        if (!active_.load()) {
            vTaskDelay(kIdleDelay);
            continue;
        }

        if (!MountSdCard()) {
            char detail[96];
            snprintf(detail, sizeof(detail), "mount err=%s try=%lu",
                     esp_err_to_name(last_mount_error_),
                     static_cast<unsigned long>(mount_attempts_));
            SetUiState("SD card not ready", detail);
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        if (refresh_requested_.exchange(false) || photos_.empty()) {
            if (!ScanPhotos()) {
                SetUiState("No photos", "Checked /photos and /PHOTOS");
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
        }

        if (current_index_ >= photos_.size()) {
            current_index_ = 0;
        }

        Frame& frame = frames_[frame_slot_];
        uint16_t width = 0;
        uint16_t height = 0;
        const std::string path = photos_[current_index_];
        if (DecodePhoto(path, frame, width, height)) {
            char title[48];
            char detail[96];
            snprintf(title, sizeof(title), "Photo %u / %u",
                     static_cast<unsigned>(current_index_ + 1),
                     static_cast<unsigned>(photos_.size()));
            snprintf(detail, sizeof(detail), "%ux%u %s",
                     static_cast<unsigned>(width),
                     static_cast<unsigned>(height),
                     path.substr(path.find_last_of('/') + 1).c_str());
            SetUiFrame(&frame.dsc, frame.background_data ? &frame.background_dsc : nullptr, title, detail);
            frame_slot_ = (frame_slot_ + 1) % 2;
            current_index_ = (current_index_ + 1) % photos_.size();
        } else {
            SetUiState("Decode failed", path.c_str());
            current_index_ = (current_index_ + 1) % photos_.size();
        }

        TickType_t waited = 0;
        while (active_.load() && waited < kSlideDelay && !refresh_requested_.load()) {
            vTaskDelay(pdMS_TO_TICKS(200));
            waited += pdMS_TO_TICKS(200);
        }
    }
}

bool PhotoService::MountSdCard() {
    ++mount_attempts_;
    mounted_ = Tab5SdReady();
    last_mount_error_ = mounted_ ? ESP_OK : ESP_ERR_NOT_FOUND;
    return mounted_;
}

bool PhotoService::ScanPhotos() {
    photos_.clear();
    current_index_ = 0;

    for (const char* dir : kPhotoDirs) {
        ScanPhotoDir(dir);
    }

    std::sort(photos_.begin(), photos_.end());
    photos_.erase(std::unique(photos_.begin(), photos_.end()), photos_.end());
    ESP_LOGI(TAG, "photo scan found %u jpg files", static_cast<unsigned>(photos_.size()));
    return !photos_.empty();
}

void PhotoService::ScanPhotoDir(const char* dir_path) {
    DIR* dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGI(TAG, "skip photo dir: %s errno=%d %s", dir_path, errno, strerror(errno));
        return;
    }

    uint32_t entries = 0;
    uint32_t jpg_count = 0;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        ++entries;
        if (is_hidden_or_resource_file(entry->d_name) || !ends_with_photo_ext(entry->d_name)) {
            if (entries <= 12) {
                ESP_LOGD(TAG, "skip photo entry dir=%s name=%s", dir_path, entry->d_name);
            }
            continue;
        }

        std::string path = std::string(dir_path) + "/" + entry->d_name;
        struct stat st = {};
        if (stat(path.c_str(), &st) != 0) {
            ESP_LOGW(TAG, "photo candidate stat failed: %s errno=%d %s", path.c_str(), errno, strerror(errno));
            continue;
        }
        if (S_ISDIR(st.st_mode) || st.st_size <= 0) {
            continue;
        }

        photos_.push_back(path);
        ++jpg_count;
        if (jpg_count <= 5) {
            ESP_LOGI(TAG, "photo candidate: %s size=%u", path.c_str(), static_cast<unsigned>(st.st_size));
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "photo dir scanned: %s entries=%lu jpg=%lu",
             dir_path, static_cast<unsigned long>(entries), static_cast<unsigned long>(jpg_count));
}

bool PhotoService::DecodePhoto(const std::string& path, Frame& frame, uint16_t& width, uint16_t& height) {
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0 || st.st_size <= 0) {
        ESP_LOGW(TAG, "photo stat failed: %s errno=%d %s", path.c_str(), errno, strerror(errno));
        return false;
    }
    if (static_cast<size_t>(st.st_size) > kMaxInputBytes) {
        ESP_LOGW(TAG, "photo too large: %s size=%u", path.c_str(), static_cast<unsigned>(st.st_size));
        return false;
    }

    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        ESP_LOGW(TAG, "photo open failed: %s errno=%d %s", path.c_str(), errno, strerror(errno));
        return false;
    }

    uint8_t* input = static_cast<uint8_t*>(heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!input) {
        fclose(file);
        ESP_LOGE(TAG, "photo input alloc failed size=%u", static_cast<unsigned>(st.st_size));
        return false;
    }

    const size_t read = fread(input, 1, st.st_size, file);
    fclose(file);
    if (read != static_cast<size_t>(st.st_size)) {
        heap_caps_free(input);
        ESP_LOGW(TAG, "photo read failed: %s read=%u size=%u",
                 path.c_str(), static_cast<unsigned>(read), static_cast<unsigned>(st.st_size));
        return false;
    }

    esp_jpeg_image_cfg_t info_cfg = {
        .indata = input,
        .indata_size = static_cast<uint32_t>(st.st_size),
        .outbuf = nullptr,
        .outbuf_size = 0,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = {
            .swap_color_bytes = 0,
        },
        .advanced = {
            .working_buffer = nullptr,
            .working_buffer_size = 0,
        },
        .priv = {},
    };

    esp_jpeg_image_output_t info = {};
    esp_err_t err = esp_jpeg_get_image_info(&info_cfg, &info);
    if (err != ESP_OK || info.width == 0 || info.height == 0) {
        heap_caps_free(input);
        ESP_LOGW(TAG, "jpeg info failed: %s err=%s", path.c_str(), esp_err_to_name(err));
        return false;
    }

    info_cfg.out_scale = choose_scale(info.width, info.height);
    esp_jpeg_image_output_t out = {};
    err = esp_jpeg_get_image_info(&info_cfg, &out);
    if (err != ESP_OK || out.output_len == 0 || out.output_len > kMaxOutputBytes) {
        heap_caps_free(input);
        ESP_LOGW(TAG, "jpeg scaled info failed: %s err=%s len=%u",
                 path.c_str(), esp_err_to_name(err), static_cast<unsigned>(out.output_len));
        return false;
    }

    uint8_t* output = static_cast<uint8_t*>(heap_caps_malloc(out.output_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!output) {
        heap_caps_free(input);
        ESP_LOGE(TAG, "photo output alloc failed len=%u", static_cast<unsigned>(out.output_len));
        return false;
    }

    esp_jpeg_image_cfg_t decode_cfg = info_cfg;
    decode_cfg.outbuf = output;
    decode_cfg.outbuf_size = out.output_len;
    err = esp_jpeg_decode(&decode_cfg, &out);
    heap_caps_free(input);
    if (err != ESP_OK) {
        heap_caps_free(output);
        ESP_LOGW(TAG, "jpeg decode failed: %s err=%s", path.c_str(), esp_err_to_name(err));
        return false;
    }

    FreeFrame(frame);
    memset(&frame.dsc, 0, sizeof(frame.dsc));
    frame.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    frame.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    frame.dsc.header.flags = LV_IMAGE_FLAGS_ALLOCATED;
    frame.dsc.header.w = out.width;
    frame.dsc.header.h = out.height;
    frame.dsc.header.stride = out.width * 2;
    frame.dsc.data_size = out.output_len;
    frame.dsc.data = output;
    frame.data = output;
    frame.data_size = out.output_len;
    width = out.width;
    height = out.height;

    if (height > width) {
        uint8_t* background = static_cast<uint8_t*>(heap_caps_malloc(kBackgroundBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (background) {
            fill_portrait_background(output, out.width, out.height, background);
            memset(&frame.background_dsc, 0, sizeof(frame.background_dsc));
            frame.background_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            frame.background_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
            frame.background_dsc.header.flags = LV_IMAGE_FLAGS_ALLOCATED;
            frame.background_dsc.header.w = kDesktopWidth;
            frame.background_dsc.header.h = kDesktopHeight;
            frame.background_dsc.header.stride = kDesktopWidth * 2;
            frame.background_dsc.data_size = kBackgroundBytes;
            frame.background_dsc.data = background;
            frame.background_data = background;
            frame.background_data_size = kBackgroundBytes;
        } else {
            ESP_LOGW(TAG, "photo portrait background alloc failed");
        }
    }

    ESP_LOGI(TAG, "photo decoded %s %ux%u len=%u scale=%d",
             path.c_str(), static_cast<unsigned>(width), static_cast<unsigned>(height),
             static_cast<unsigned>(out.output_len), static_cast<int>(info_cfg.out_scale));
    return true;
}

void PhotoService::FreeFrame(Frame& frame) {
    if (frame.data) {
        heap_caps_free(frame.data);
        frame.data = nullptr;
    }
    frame.data_size = 0;
    memset(&frame.dsc, 0, sizeof(frame.dsc));
    if (frame.background_data) {
        heap_caps_free(frame.background_data);
        frame.background_data = nullptr;
    }
    frame.background_data_size = 0;
    memset(&frame.background_dsc, 0, sizeof(frame.background_dsc));
}

void PhotoService::ClearFrames() {
    FreeFrame(frames_[0]);
    FreeFrame(frames_[1]);
}

void PhotoService::SetUiState(const char* title, const char* detail) {
    if (!desktop_ui_) {
        return;
    }
    if (lvgl_port_lock(100)) {
        desktop_ui_->SetPhotoState(title, detail);
        lvgl_port_unlock();
    }
}

void PhotoService::SetUiFrame(const lv_img_dsc_t* frame, const lv_img_dsc_t* background,
                              const char* title, const char* detail) {
    if (!desktop_ui_) {
        return;
    }
    if (lvgl_port_lock(100)) {
        desktop_ui_->SetPhotoFrame(frame, background, title, detail);
        lvgl_port_unlock();
    }
}
