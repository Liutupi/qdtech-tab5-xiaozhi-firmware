#include "fc_emulator_service.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "tab5_sd.h"
#include "qdtech_nofrendo.h"
#include "usb_gamepad_host.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_lvgl_port.h>
#include <esp_random.h>
#include <freertos/idf_additions.h>

#include "tab5_rom_names.h"

#define TAG "FcEmulator"

namespace {
constexpr const char* kRomDirs[] = {
    "/sdcard/FC",
    "/sdcard/nes",
    "/sdcard/roms",
    "/sdcard",
};
constexpr size_t kMaxScannedRoms = 192;
constexpr long kMaxNofrendoRomBytes = 2 * 1024 * 1024;
constexpr uint16_t kNesFrameWidth = 256;
constexpr uint16_t kNesFrameHeight = 240;
constexpr uint16_t kFrameWidth = 360;
constexpr uint16_t kFrameHeight = 154;
constexpr size_t kFramePixels = kFrameWidth * kFrameHeight;
constexpr TickType_t kButtonReleaseHold = pdMS_TO_TICKS(120);
constexpr TickType_t kIdleDelay = pdMS_TO_TICKS(250);
constexpr TickType_t kFrameDelay = pdMS_TO_TICKS(1);
constexpr int64_t kLvglFramePublishIntervalUs = 50000;
constexpr int64_t kDirectFramePublishIntervalUs = 33333;

bool ends_with_nes_ext(const char* name) {
    if (!name) {
        return false;
    }
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower.ends_with(".nes");
}

bool is_hidden_or_resource_file(const char* name) {
    if (!name || name[0] == '\0') {
        return true;
    }
    return name[0] == '.' || (name[0] == '_' && name[1] == '.');
}

std::string file_name_from_path(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void strip_nes_extension(std::string& name) {
    if (name.size() >= 4 && ends_with_nes_ext(name.c_str())) {
        name.resize(name.size() - 4);
    }
}

void trim_ascii_space(std::string& text) {
    while (!text.empty() && static_cast<unsigned char>(text.back()) <= ' ') {
        text.pop_back();
    }
    size_t start = 0;
    while (start < text.size() && static_cast<unsigned char>(text[start]) <= ' ') {
        ++start;
    }
    if (start > 0) {
        text.erase(0, start);
    }
}

size_t utf8_char_len(unsigned char c) {
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

std::string utf8_truncate(const std::string& text, size_t max_chars) {
    size_t pos = 0;
    size_t chars = 0;
    while (pos < text.size() && chars < max_chars) {
        const size_t len = utf8_char_len(static_cast<unsigned char>(text[pos]));
        if (pos + len > text.size()) {
            break;
        }
        pos += len;
        ++chars;
    }
    if (pos >= text.size()) {
        return text;
    }
    return text.substr(0, pos) + "~";
}

// "106.冒险岛2无限人.nes" -> "冒险岛2无限人" (see tab5_rom_names.h). Chinese file names need
// CONFIG_FATFS_API_ENCODING_UTF_8; with an OEM code page only the digits survived.
std::string rom_display_name(const std::string& path, size_t max_chars = 22) {
    return utf8_truncate(tab5_roms::TidyTitle(file_name_from_path(path)), max_chars);
}

void strip_utf8_bom(std::string& text) {
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xef &&
        static_cast<unsigned char>(text[1]) == 0xbb &&
        static_cast<unsigned char>(text[2]) == 0xbf) {
        text.erase(0, 3);
    }
}

void unquote_ascii(std::string& text) {
    if (text.size() >= 2 &&
        ((text.front() == '"' && text.back() == '"') ||
         (text.front() == '\'' && text.back() == '\''))) {
        text = text.substr(1, text.size() - 2);
        trim_ascii_space(text);
    }
}

std::string rom_alias_key(const std::string& path_or_name) {
    std::string key = file_name_from_path(path_or_name);
    strip_nes_extension(key);
    trim_ascii_space(key);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return c < 0x80 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c);
    });
    return key;
}

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

int16_t clamp16(int value) {
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return static_cast<int16_t>(value);
}

bool is_supported_nofrendo_mapper(uint8_t mapper) {
    static constexpr uint8_t kSupportedMappers[] = {
        0, 1, 2, 3, 4, 5, 7, 8, 9, 11, 15, 16, 18, 19,
        21, 22, 23, 24, 25, 32, 33, 34, 40, 41, 42, 46,
        50, 64, 65, 66, 70, 73, 75, 78, 79, 83, 85, 87, 93,
        94, 99, 160, 198, 224, 229, 231,
    };
    for (uint8_t supported : kSupportedMappers) {
        if (supported == mapper) {
            return true;
        }
    }
    return false;
}

uint8_t ines_mapper(const uint8_t header[16]) {
    return static_cast<uint8_t>((header[6] >> 4) | (header[7] & 0xf0));
}

bool is_nes2_header(const uint8_t header[16]) {
    return (header[7] & 0x0c) == 0x08;
}

uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

uint8_t corrected_mapper_for_crc(uint8_t mapper, uint32_t crc) {
    if (mapper != 4) {
        return mapper;
    }
    if (crc == 0xfdec419f || crc == 0x78292584) {
        return 83;
    }
    if (crc == 0x9b518d54 || crc == 0x91396b3f || crc == 0xaa621fa0 || crc == 0x48d1f54a) {
        return 224;
    }
    if (crc == 0x3963f12a) {
        return 198;
    }
    return mapper;
}

} // namespace

void FcEmulatorService::Start(UiSink sink) {
    ui_sink_ = std::move(sink);
    ESP_LOGI(TAG, "fc emulator service registered");
}

void FcEmulatorService::SetDirectFrameCallback(DirectFrameCallback callback) {
    direct_frame_cb_ = std::move(callback);
}

void FcEmulatorService::SetIndexedFrameCallback(IndexedFrameCallback callback) {
    indexed_frame_cb_ = std::move(callback);
}

void FcEmulatorService::SetVideoSessionHooks(std::function<void()> begin, std::function<void()> end) {
    video_begin_hook_ = std::move(begin);
    video_end_hook_ = std::move(end);
}

void FcEmulatorService::SetActive(bool active) {
    const bool previous = active_.exchange(active);
    if (previous != active) {
        ESP_LOGI(TAG, "fc page %s", active ? "active" : "inactive");
        if (active) {
            Application::GetInstance().SetExternalAudioActive(true);
            EnsureTaskStarted();
            playing_.store(false);
            PublishMode(false);
            if (roms_.empty()) {
                scan_requested_.store(true);
            } else {
                PublishState("Select ROM", SelectedName().c_str());
            }
        } else {
            playing_.store(false);
            qd_nofrendo_request_stop();
            controller_state_.store(0);
            controller_release_tick_.store(0);
            scan_requested_.store(false);
            start_requested_.store(false);
            release_roms_requested_.store(true);
            PublishMode(false);
            Application::GetInstance().SetExternalAudioActive(false);
        }
    }
}

bool FcEmulatorService::PrepareSdCard() {
    return MountSdCard();
}

void FcEmulatorService::PrepareTask() {
    EnsureTaskStarted();
}

void FcEmulatorService::PlayPause() {
    EnsureTaskStarted();
    if (scanning_.load()) {
        PublishMode(false);
        PublishState("Scanning", "Please wait");
        return;
    }
    if (playing_.load()) {
        Stop();
        return;
    }
    if (roms_.empty()) {
        scan_requested_.store(true);
        play_after_scan_.store(true);
        PublishMode(false);
        PublishState("Scanning", "Looking for .nes files");
        return;
    }
    PublishMode(true);
    PublishState("Loading ROM", SelectedName().c_str());
    start_requested_.store(true);
    ESP_LOGI(TAG, "fc start requested rom=%s", SelectedName().c_str());
}

void FcEmulatorService::Stop() {
    start_requested_.store(false);
    play_after_stop_.store(false);
    if (playing_.load()) {
        // Only signal the emu thread. Touching LVGL / publishing from here
        // while qd_nofrendo_run is still on the stack causes Load access fault.
        qd_nofrendo_request_stop();
        ESP_LOGI(TAG, "fc stop requested (async)");
        return;
    }
    qd_nofrendo_request_stop();
    playing_.store(false);
    controller_state_.store(0);
    controller_release_tick_.store(0);
    PublishMode(false);
    PublishState(roms_.empty() ? "No ROM" : "Select ROM",
                 roms_.empty() ? "Put .nes files in /nes" : SelectedName().c_str());
    ESP_LOGI(TAG, "fc stop/list");
}

void FcEmulatorService::Next() {
    // Never change ROM while nofrendo is running — that faults the emu task.
    if (playing_.load()) {
        ESP_LOGW(TAG, "fc next ignored while playing");
        return;
    }
    EnsureTaskStarted();
    if (scanning_.load()) {
        PublishMode(false);
        PublishState("Scanning", "Please wait");
        return;
    }
    if (roms_.empty()) {
        scan_requested_.store(true);
        PublishMode(false);
        PublishState("Scanning", "Looking for .nes files");
        return;
    }
    playing_.store(false);
    start_requested_.store(false);
    qd_nofrendo_request_stop();
    controller_state_.store(0);
    controller_release_tick_.store(0);
    size_t idx = selected_index_.load(std::memory_order_relaxed);
    idx = (idx + 1) % roms_.size();
    selected_index_.store(idx, std::memory_order_relaxed);
    ESP_LOGI(TAG, "fc next index=%u rom=%s", static_cast<unsigned>(idx), SelectedName().c_str());
    PublishMode(false);
    PublishState("Select ROM", SelectedName().c_str());
}

void FcEmulatorService::Prev() {
    if (playing_.load()) {
        ESP_LOGW(TAG, "fc prev ignored while playing");
        return;
    }
    EnsureTaskStarted();
    if (scanning_.load()) {
        PublishMode(false);
        PublishState("Scanning", "Please wait");
        return;
    }
    if (roms_.empty()) {
        scan_requested_.store(true);
        PublishMode(false);
        PublishState("Scanning", "Looking for .nes files");
        return;
    }
    playing_.store(false);
    start_requested_.store(false);
    qd_nofrendo_request_stop();
    controller_state_.store(0);
    controller_release_tick_.store(0);
    size_t idx = selected_index_.load(std::memory_order_relaxed);
    idx = idx == 0 ? roms_.size() - 1 : idx - 1;
    selected_index_.store(idx, std::memory_order_relaxed);
    ESP_LOGI(TAG, "fc prev index=%u rom=%s", static_cast<unsigned>(idx), SelectedName().c_str());
    PublishMode(false);
    PublishState("Select ROM", SelectedName().c_str());
}

void FcEmulatorService::EnsureTaskStarted() {
    if (task_handle_) {
        return;
    }

    // -O2 nofrendo inlines more; keep headroom (stack lives in PSRAM).
    constexpr uint32_t stack_size = 8192;
    // Was 1 (below LVGL/audio helpers) which starved emulation. The emu loop
    // now sleeps between frames (osd_idle), so a mid priority is safe.
    constexpr UBaseType_t task_priority = 3;
    ESP_LOGI(TAG, "fc task create free_internal=%u largest_internal=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));

    BaseType_t ret = xTaskCreateWithCaps(
        TaskWrapper, "fc_emulator", stack_size, this, task_priority, &task_handle_,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret == pdPASS) {
        ESP_LOGI(TAG, "fc task started stack=%u memory=psram", static_cast<unsigned>(stack_size));
        return;
    }

    ESP_LOGW(TAG, "fc task PSRAM create failed ret=%ld, trying internal memory", static_cast<long>(ret));
    task_handle_ = nullptr;
    const BaseType_t freertos_ret = xTaskCreate(
        TaskWrapper, "fc_emulator", stack_size, this, task_priority, &task_handle_);
    if (freertos_ret != pdPASS) {
        task_handle_ = nullptr;
        ESP_LOGE(TAG, "fc task create failed ret=%ld", static_cast<long>(freertos_ret));
        PublishState("FC unavailable", "Not enough memory");
        return;
    }
    ESP_LOGI(TAG, "fc task started stack=%u memory=internal", static_cast<unsigned>(stack_size));
}

void FcEmulatorService::TaskWrapper(void* arg) {
    static_cast<FcEmulatorService*>(arg)->TaskLoop();
}

void FcEmulatorService::PollPadOnFrame() {
    // Refresh USB pad every frame so NES sees live input.
    const uint8_t pad = static_cast<uint8_t>(controller_state_.load() | UsbGamepadNesMask());
    qd_nofrendo_set_controller(pad);
    // Select+Start together leaves the game (Select alone now reaches the
    // game; many titles need it). Works even while LVGL is paused.
    constexpr uint8_t kExitCombo = 0x04 | 0x08;
    if ((pad & kExitCombo) == kExitCombo && !exit_combo_latched_) {
        exit_combo_latched_ = true;
        ESP_LOGI(TAG, "fc exit combo (Select+Start)");
        qd_nofrendo_request_stop();
    }
}

int FcEmulatorService::NofrendoIndexedFrameThunk(const uint8_t* const* lines, const uint16_t* palette,
                                                 uint16_t width, uint16_t height, void* user) {
    auto* self = static_cast<FcEmulatorService*>(user);
    if (!self || !self->indexed_frame_cb_ || !self->playing_.load()) return 0;
    if (!self->indexed_frame_cb_(lines, palette, width, height)) return 0;
    self->PollPadOnFrame();
    return 1;
}

int FcEmulatorService::NofrendoFrameThunk(const uint16_t* pixels, uint16_t width, uint16_t height, void* user) {
    auto* self = static_cast<FcEmulatorService*>(user);
    if (!self) return 0;
    self->PollPadOnFrame();
    return self->PublishDirectFrame(pixels, width, height) ? 1 : 0;
}

void FcEmulatorService::NofrendoAudioThunk(const int16_t* samples, int sample_count, int sample_rate, void* user) {
    auto* self = static_cast<FcEmulatorService*>(user);
    if (self) {
        self->WriteNofrendoAudio(samples, sample_count, sample_rate);
    }
}

void FcEmulatorService::TaskLoop() {
    PublishMode(false);
    PublishState("FC Emulator", "Open /nes on SD card");

    while (true) {
        const bool is_active = active_.load();
        if (release_roms_requested_.exchange(false)) {
            roms_.clear();
            roms_.shrink_to_fit();
            rom_aliases_.clear();
            rom_aliases_.shrink_to_fit();
            selected_index_.store(0, std::memory_order_relaxed);
            ESP_LOGI(TAG, "fc rom list released");
        }
        if (!is_active && !scan_requested_.load()) {
            vTaskDelay(kIdleDelay);
            continue;
        }

        if (!MountSdCard()) {
            ++sd_mount_failures_;
            playing_.store(false);
            PublishMode(false);
            PublishState("SD card not ready", "Insert FAT SD with .nes files");
            if (!is_active && sd_mount_failures_ >= 1) {
                scan_requested_.store(false);
            }
            vTaskDelay(pdMS_TO_TICKS(sd_mount_failures_ < 3 ? 1500 : 5000));
            continue;
        }
        sd_mount_failures_ = 0;

        if (scan_requested_.exchange(false) || roms_.empty()) {
            if (!ScanRoms()) {
                playing_.store(false);
                PublishMode(false);
                PublishState("No NES ROMs", "Checked /nes, /FC, /roms");
                vTaskDelay(pdMS_TO_TICKS(1500));
                continue;
            }
            playing_.store(false);
            controller_state_.store(0);
            controller_release_tick_.store(0);
            PublishMode(false);
            PublishState("Select ROM", SelectedName().c_str());
            // List the ROMs; the user picks one on the game page.
            play_after_scan_.store(false);
        }

        if (!is_active) {
            vTaskDelay(kIdleDelay);
            continue;
        }

        if (start_requested_.exchange(false)) {
            PublishMode(true);
            PublishState("Loading ROM", SelectedName().c_str());
            if (!ValidateSelectedRom()) {
                playing_.store(false);
                PublishMode(false);
                PublishState("Unsupported ROM", SelectedName().c_str());
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            frame_counter_ = 0;
            produced_frame_counter_ = 0;
            last_perf_frame_counter_ = 0;
            last_perf_log_us_ = esp_timer_get_time();
            controller_state_.store(0);
            controller_release_tick_.store(0);
            playing_.store(true);
            PublishMode(true);
            PublishState(SelectedName().c_str(), "Playing");
            ESP_LOGI(TAG, "fc play rom=%s", SelectedName().c_str());
        }

        if (!playing_.load()) {
            vTaskDelay(kIdleDelay);
            continue;
        }

        RunNofrendoRom();
    }
}

bool FcEmulatorService::MountSdCard() {
    mounted_ = Tab5SdReady();
    return mounted_;
}

bool FcEmulatorService::ScanRoms() {
    scanning_.store(true);
    roms_.clear();
    rom_aliases_.clear();
    roms_.reserve(kMaxScannedRoms);
    selected_index_.store(0, std::memory_order_relaxed);

    for (const char* dir : kRomDirs) {
        const size_t before = roms_.size();
        ScanRomDir(dir);
        if (roms_.size() > before) {
            ESP_LOGI(TAG, "fc using rom dir: %s", dir);
            LoadRomAliases(dir);
            break;
        }
    }

    std::sort(roms_.begin(), roms_.end());
    roms_.erase(std::unique(roms_.begin(), roms_.end()), roms_.end());
    std::sort(roms_.begin(), roms_.end(), [this](const std::string& a, const std::string& b) {
        return RomDisplayName(a, 96) < RomDisplayName(b, 96);
    });
    ESP_LOGI(TAG, "fc scan found %u nes files", static_cast<unsigned>(roms_.size()));
    const bool found = !roms_.empty();
    scanning_.store(false);
    return found;
}

size_t FcEmulatorService::ScanRomDir(const char* dir_path) {
    DIR* dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGI(TAG, "skip rom dir: %s errno=%d %s", dir_path, errno, strerror(errno));
        return 0;
    }

    uint32_t entries = 0;
    uint32_t rom_count = 0;
    struct dirent* entry = nullptr;
    while (roms_.size() < kMaxScannedRoms && (entry = readdir(dir)) != nullptr) {
        ++entries;
        if (is_hidden_or_resource_file(entry->d_name) || !ends_with_nes_ext(entry->d_name)) {
            continue;
        }

        std::string path = std::string(dir_path) + "/" + entry->d_name;
        roms_.push_back(path);
        ++rom_count;
        if (rom_count == 1) {
            ESP_LOGI(TAG, "first rom candidate found in %s", dir_path);
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "rom dir scanned: %s entries=%lu nes=%lu%s",
             dir_path, static_cast<unsigned long>(entries), static_cast<unsigned long>(rom_count),
             roms_.size() >= kMaxScannedRoms ? " capped" : "");
    return rom_count;
}

void FcEmulatorService::LoadRomAliases(const char* dir_path) {
    if (!dir_path || dir_path[0] == '\0') {
        return;
    }

    const std::string dir(dir_path);
    const std::string files[] = {
        dir + "/roms.txt",
        dir + "/roms.csv",
        dir + "/games.txt",
    };
    for (const auto& file : files) {
        if (LoadRomAliasFile(file.c_str())) {
            return;
        }
    }
}

bool FcEmulatorService::LoadRomAliasFile(const char* path) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        return false;
    }

    char line[256];
    uint32_t line_count = 0;
    uint32_t alias_count = 0;
    while (alias_count < kMaxScannedRoms && fgets(line, sizeof(line), file)) {
        ++line_count;
        std::string row(line);
        strip_utf8_bom(row);
        trim_ascii_space(row);
        if (row.empty() || row[0] == '#' || row[0] == ';') {
            continue;
        }

        const size_t sep = row.find_first_of("\t=,");
        if (sep == std::string::npos) {
            continue;
        }
        std::string key = row.substr(0, sep);
        std::string alias = row.substr(sep + 1);
        trim_ascii_space(key);
        trim_ascii_space(alias);
        unquote_ascii(key);
        unquote_ascii(alias);
        key = rom_alias_key(key);
        if (key.empty() || alias.empty()) {
            continue;
        }

        auto existing = std::find_if(rom_aliases_.begin(), rom_aliases_.end(),
                                     [&key](const auto& item) { return item.first == key; });
        if (existing == rom_aliases_.end()) {
            rom_aliases_.push_back({key, alias});
        } else {
            existing->second = alias;
        }
        ++alias_count;
    }
    fclose(file);
    ESP_LOGI(TAG, "rom aliases loaded: %s lines=%lu aliases=%lu",
             path, static_cast<unsigned long>(line_count), static_cast<unsigned long>(rom_aliases_.size()));
    return !rom_aliases_.empty();
}

bool FcEmulatorService::ValidateSelectedRom() {
    const size_t idx = selected_index_.load(std::memory_order_relaxed);
    if (roms_.empty() || idx >= roms_.size()) {
        PublishState("No ROM", "Put .nes files in /nes");
        return false;
    }

    FILE* file = fopen(roms_[idx].c_str(), "rb");
    if (!file) {
        PublishState("Open failed", SelectedName().c_str());
        ESP_LOGW(TAG, "rom open failed: %s errno=%d %s", roms_[idx].c_str(), errno, strerror(errno));
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        PublishState("Open failed", "Could not read ROM size");
        ESP_LOGW(TAG, "rom seek failed: %s errno=%d %s", roms_[idx].c_str(), errno, strerror(errno));
        return false;
    }
    const long size = ftell(file);
    if (size <= 16 || size > kMaxNofrendoRomBytes) {
        fclose(file);
        char detail[48];
        snprintf(detail, sizeof(detail), "Size %ldKB > 2048KB", size > 0 ? size / 1024 : 0);
        PublishState("ROM too large", detail);
        ESP_LOGW(TAG, "rom size unsupported: %s size=%ld max=%ld",
                 roms_[idx].c_str(), size, kMaxNofrendoRomBytes);
        return false;
    }
    rewind(file);
    uint8_t header[16] = {};
    const size_t read = fread(header, 1, sizeof(header), file);
    if (read != sizeof(header) || memcmp(header, "NES\x1a", 4) != 0) {
        fclose(file);
        PublishState("Bad ROM", "Missing iNES header");
        ESP_LOGW(TAG, "rom header invalid: %s", roms_[idx].c_str());
        return false;
    }
    uint32_t crc = 0xffffffffu;
    uint8_t buffer[512];
    const bool has_trainer = (header[6] & 0x04) != 0;
    if (has_trainer) {
        if (fread(buffer, 1, 512, file) != 512) {
            fclose(file);
            PublishState("Bad ROM", "Short trainer");
            ESP_LOGW(TAG, "rom trainer short: %s", roms_[idx].c_str());
            return false;
        }
    }
    const size_t payload_size = static_cast<size_t>(header[4]) * 16 * 1024 +
                                static_cast<size_t>(header[5]) * 8 * 1024;
    size_t remaining = payload_size;
    while (remaining > 0) {
        const size_t chunk = std::min(remaining, sizeof(buffer));
        const size_t got = fread(buffer, 1, chunk, file);
        if (got != chunk) {
            fclose(file);
            PublishState("Bad ROM", "Short payload");
            ESP_LOGW(TAG, "rom payload short: %s got=%u need=%u",
                     roms_[idx].c_str(), static_cast<unsigned>(got), static_cast<unsigned>(chunk));
            return false;
        }
        crc = crc32_update(crc, buffer, got);
        remaining -= got;
    }
    crc = ~crc;
    fclose(file);
    const uint8_t mapper = ines_mapper(header);
    const uint8_t corrected_mapper = corrected_mapper_for_crc(mapper, crc);
    ESP_LOGI(TAG, "rom diag path=%s prg=%u chr=%u header_mapper=%u corrected_mapper=%u crc=%08lx size=%ld",
             roms_[idx].c_str(), header[4], header[5], static_cast<unsigned>(mapper),
             static_cast<unsigned>(corrected_mapper), static_cast<unsigned long>(crc), size);
    if (is_nes2_header(header)) {
        PublishState("Unsupported ROM", "NES 2.0 header");
        ESP_LOGW(TAG, "rom uses NES 2.0 header: %s mapper=%u", roms_[idx].c_str(),
                 static_cast<unsigned>(mapper));
        return false;
    }
    if (!is_supported_nofrendo_mapper(corrected_mapper)) {
        char detail[48];
        snprintf(detail, sizeof(detail), "Mapper %u not supported", static_cast<unsigned>(corrected_mapper));
        PublishState("Unsupported ROM", detail);
        ESP_LOGW(TAG, "rom mapper unsupported: %s header_mapper=%u corrected_mapper=%u prg=%u chr=%u crc=%08lx",
                 roms_[idx].c_str(), static_cast<unsigned>(mapper), static_cast<unsigned>(corrected_mapper),
                 header[4], header[5], static_cast<unsigned long>(crc));
        return false;
    }
    ESP_LOGI(TAG, "rom header ok prg=%u chr=%u mapper=%u corrected=%u size=%ld crc=%08lx",
             header[4], header[5], static_cast<unsigned>(mapper),
             static_cast<unsigned>(corrected_mapper), size, static_cast<unsigned long>(crc));
    return true;
}

void FcEmulatorService::RunNofrendoRom() {
    const size_t idx = selected_index_.load(std::memory_order_relaxed);
    if (roms_.empty() || idx >= roms_.size()) {
        playing_.store(false);
        PublishMode(false);
        PublishState("No ROM", "Put .nes files in /nes");
        return;
    }

    exit_combo_latched_ = false;
    if (video_begin_hook_) {
        video_begin_hook_();
    }
    qd_nofrendo_set_indexed_frame_callback(NofrendoIndexedFrameThunk, this);
    qd_nofrendo_set_frame_callback(NofrendoFrameThunk, this);
    qd_nofrendo_set_audio_callback(NofrendoAudioThunk, this);
    qd_nofrendo_set_controller(static_cast<uint8_t>(controller_state_.load() | UsbGamepadNesMask()));
    audio_frame_counter_ = 0;
    last_audio_log_us_ = 0;
    ESP_LOGI(TAG, "nofrendo run rom=%s free_internal=%u largest_internal=%u",
             SelectedName().c_str(),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    const int result = qd_nofrendo_run(roms_[idx].c_str());
    qd_nofrendo_set_indexed_frame_callback(nullptr, nullptr);
    qd_nofrendo_set_frame_callback(nullptr, nullptr);
    qd_nofrendo_set_audio_callback(nullptr, nullptr);
    controller_state_.store(0);
    controller_release_tick_.store(0);
    playing_.store(false);
    audio_output_buf_.clear();
    // Give the panel back to LVGL before any UI publishing below.
    if (video_end_hook_) {
        video_end_hook_();
    }

    ESP_LOGI(TAG, "nofrendo finished result=%d", result);
    PublishMode(false);
    if (active_.load()) {
        PublishState(result == 0 ? "Select ROM" : "Load failed",
                     result == 0 ? SelectedName().c_str() : "Nofrendo could not run ROM");
    }
    // Select → list → A: start the newly picked ROM now that the old one is idle.
    if (play_after_stop_.exchange(false) && active_.load()) {
        start_requested_.store(true);
        ESP_LOGI(TAG, "fc queued start after stop rom=%s", SelectedName().c_str());
    }
}

void FcEmulatorService::FreeFrame(Frame& frame) {
    if (frame.pixels) {
        heap_caps_free(frame.pixels);
        frame.pixels = nullptr;
    }
    frame.pixel_count = 0;
    memset(&frame.dsc, 0, sizeof(frame.dsc));
}

void FcEmulatorService::ClearFrames() {
    FreeFrame(frames_[0]);
    FreeFrame(frames_[1]);
}

void FcEmulatorService::PublishState(const char* title, const char* detail) {
    if (!ui_sink_.set_state) {
        return;
    }
    const std::string list = BuildRomList();
    if (lvgl_port_lock(100)) {
        ui_sink_.set_state(title, detail, list.c_str());
        lvgl_port_unlock();
    }
}

void FcEmulatorService::PublishMode(bool playing) {
    if (!ui_sink_.set_mode) {
        return;
    }
    if (lvgl_port_lock(100)) {
        ui_sink_.set_mode(playing);
        lvgl_port_unlock();
    }
}

bool FcEmulatorService::PublishFrame(Frame& frame) {
    constexpr size_t kNesPixels = kNesFrameWidth * kNesFrameHeight;
    if (direct_frame_cb_ && playing_.load() && frame.pixels && frame.pixel_count == kNesPixels) {
        return direct_frame_cb_(frame.pixels, kNesFrameWidth, kNesFrameHeight);
    }
    return PublishLvglFrame(&frame.dsc);
}

bool FcEmulatorService::PublishLvglFrame(const lv_img_dsc_t* frame) {
    if (!ui_sink_.set_frame || !frame || !frame->data) {
        return false;
    }
    if (lvgl_port_lock(50)) {
        ui_sink_.set_frame(frame);
        lvgl_port_unlock();
        return true;
    }
    return false;
}

bool FcEmulatorService::PublishDirectFrame(const uint16_t* pixels, uint16_t width, uint16_t height) {
    static int64_t last_log_us = 0;
    static uint32_t frames = 0;
    if (!direct_frame_cb_ || !playing_.load() || !pixels || width == 0 || height == 0) {
        return false;
    }

    ++frames;
    const int64_t now = esp_timer_get_time();
    if (last_log_us == 0) {
        last_log_us = now;
    } else if (now - last_log_us >= 1000000) {
        uint16_t min_pixel = 0xffff;
        uint16_t max_pixel = 0x0000;
        uint32_t non_black = 0;
        const size_t sample_count = static_cast<size_t>(width) * height;
        const size_t sample_step = std::max<size_t>(1, sample_count / 512);
        for (size_t i = 0; i < sample_count; i += sample_step) {
            const uint16_t pixel = pixels[i];
            min_pixel = std::min(min_pixel, pixel);
            max_pixel = std::max(max_pixel, pixel);
            if (pixel != 0x0000) {
                ++non_black;
            }
        }
        ESP_LOGI(TAG, "nofrendo fps=%lu frame=%ux%u min=%04x max=%04x nonblack_sample=%lu free_internal=%u largest_internal=%u",
                 static_cast<unsigned long>(frames),
                 static_cast<unsigned>(width),
                 static_cast<unsigned>(height),
                 static_cast<unsigned>(min_pixel),
                 static_cast<unsigned>(max_pixel),
                 static_cast<unsigned long>(non_black),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        frames = 0;
        last_log_us = now;
    }

    return direct_frame_cb_(pixels, width, height);
}


void FcEmulatorService::WriteNofrendoAudio(const int16_t* samples, int sample_count, int sample_rate) {
    if (!playing_.load() || !samples || sample_count <= 0 || sample_rate <= 0) {
        return;
    }

    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (!codec) {
        return;
    }

    auto& app = Application::GetInstance();
    if (!app.IsExternalAudioActive()) {
        app.SetExternalAudioActive(true);
    }

    const int out_rate = codec->output_sample_rate();
    if (sample_rate == out_rate) {
        audio_output_buf_.assign(samples, samples + sample_count);
    } else {
        const int out_count = std::max(1, sample_count * out_rate / sample_rate);
        audio_output_buf_.resize(out_count);
        for (int i = 0; i < out_count; ++i) {
            const int64_t src_pos_q16 = static_cast<int64_t>(i) * sample_rate * 65536 / out_rate;
            const int src_index = static_cast<int>(src_pos_q16 >> 16);
            const int frac = static_cast<int>(src_pos_q16 & 0xffff);
            if (src_index >= sample_count - 1) {
                audio_output_buf_[i] = samples[sample_count - 1];
            } else {
                const int a = samples[src_index];
                const int b = samples[src_index + 1];
                audio_output_buf_[i] = clamp16((a * (65536 - frac) + b * frac) >> 16);
            }
        }
    }

    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
    }
    codec->OutputData(audio_output_buf_);

    ++audio_frame_counter_;
    const int64_t now = esp_timer_get_time();
    if (last_audio_log_us_ == 0) {
        last_audio_log_us_ = now;
    } else if (now - last_audio_log_us_ >= 2000000) {
        ESP_LOGI(TAG, "fc audio frames=%lu samples=%u rate=%d->%d free_internal=%u largest_internal=%u",
                 static_cast<unsigned long>(audio_frame_counter_),
                 static_cast<unsigned>(audio_output_buf_.size()),
                 sample_rate, out_rate,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        audio_frame_counter_ = 0;
        last_audio_log_us_ = now;
    }
}

std::string FcEmulatorService::RomDisplayName(const std::string& path, size_t max_chars) const {
    const std::string key = rom_alias_key(path);
    auto alias = std::find_if(rom_aliases_.begin(), rom_aliases_.end(),
                              [&key](const auto& item) { return item.first == key; });
    if (alias != rom_aliases_.end()) {
        return utf8_truncate(alias->second, max_chars);
    }
    return rom_display_name(path, max_chars);
}

std::string FcEmulatorService::SelectedName() const {
    const size_t idx = selected_index_.load(std::memory_order_relaxed);
    if (roms_.empty() || idx >= roms_.size()) {
        return "No ROM";
    }
    return RomDisplayName(roms_[idx], 24);
}

void FcEmulatorService::SelectRomIndex(int index) {
    if (playing_.load()) {
        ESP_LOGW(TAG, "fc select ignored while playing");
        return;
    }
    EnsureTaskStarted();
    if (roms_.empty()) {
        scan_requested_.store(true);
        return;
    }
    if (index < 0) index = 0;
    if (index >= static_cast<int>(roms_.size())) index = static_cast<int>(roms_.size()) - 1;
    selected_index_.store(static_cast<size_t>(index), std::memory_order_relaxed);
    PublishState("Select ROM", SelectedName().c_str());
    ESP_LOGI(TAG, "fc select index=%d rom=%s", index, SelectedName().c_str());
}

void FcEmulatorService::StartSelected() {
    // Re-entering a game after Select: always re-activate and queue a clean
    // start. If the previous nofrendo is still winding down, start afterwards.
    SetActive(true);
    if (roms_.empty()) {
        scan_requested_.store(true);
        play_after_scan_.store(true);
        PublishState("Scanning", "Looking for .nes files");
        return;
    }
    if (playing_.load()) {
        ESP_LOGW(TAG, "fc start: previous rom still running, queue restart");
        play_after_stop_.store(true);
        Stop();
        return;
    }
    play_after_stop_.store(false);
    PublishMode(true);
    PublishState("Loading ROM", SelectedName().c_str());
    start_requested_.store(true);
    ESP_LOGI(TAG, "fc start requested rom=%s", SelectedName().c_str());
}

int FcEmulatorService::CurrentRomIndex() const {
    return static_cast<int>(selected_index_.load(std::memory_order_relaxed));
}

int FcEmulatorService::RomCount() const { return static_cast<int>(roms_.size()); }

std::string FcEmulatorService::RomNameAt(int index) const {
    if (index < 0 || index >= static_cast<int>(roms_.size())) return "";
    return RomDisplayName(roms_[static_cast<size_t>(index)], 28);
}

std::string FcEmulatorService::BuildRomList() const {
    if (roms_.empty()) {
        return "No .nes files found\nPut ROMs in /sdcard/nes";
    }

    const size_t idx = selected_index_.load(std::memory_order_relaxed);
    std::string text;
    constexpr size_t kMaxRows = 8;
    const size_t count = std::min<size_t>(roms_.size(), kMaxRows);
    size_t start = idx >= count ? idx - count + 1 : 0;
    if (start + count > roms_.size()) {
        start = roms_.size() - count;
    }

    char index_line[32];
    snprintf(index_line, sizeof(index_line), "%u/%u\n",
             static_cast<unsigned>(idx + 1),
             static_cast<unsigned>(roms_.size()));
    text += index_line;

    for (size_t row = 0; row < count; ++row) {
        const size_t i = start + row;
        const std::string name = RomDisplayName(roms_[i]);
        char row_prefix[12];
        snprintf(row_prefix, sizeof(row_prefix), "%03u ", static_cast<unsigned>(i + 1));
        text += (i == idx ? "> " : "  ");
        text += row_prefix;
        text += name;
        if (row + 1 < count) {
            text += "\n";
        }
    }
    return text;
}

void FcEmulatorService::SetController(uint8_t controller) {
    controller_state_.store(controller);
    controller_release_tick_.store(0);
    qd_nofrendo_set_controller(static_cast<uint8_t>(controller | UsbGamepadNesMask()));
}
