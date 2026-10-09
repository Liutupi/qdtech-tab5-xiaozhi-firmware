#include "tab5_md_launcher.h"

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstring>

#include "application.h"
#include "tab5_reset_diag.h"

namespace tab5_md {
namespace {

const char* TAG = "Tab5Md";
constexpr const char* kLaunchDir = "/sdcard/tab5/md";
constexpr const char* kLaunchFile = "/sdcard/tab5/md/launch.txt";
constexpr size_t kMaxCatalogBytes = 64 * 1024;

// The ota_0 app must be the Tab5 updater with MD support (version 1.2 or later
// (controls/audio/exit)).
bool MdAppPresent(const esp_partition_t* app) {
    esp_app_desc_t desc = {};
    if (!app || esp_ota_get_partition_description(app, &desc) != ESP_OK)
        return false;
    if (std::strncmp(desc.project_name, "tab5_updater", sizeof(desc.project_name)) != 0)
        return false;
    int major = 0, minor = 0;
    return std::sscanf(desc.version, "%d.%d", &major, &minor) == 2 &&
           (major > 1 || (major == 1 && minor >= 2));
}

}  // namespace

std::shared_ptr<const Catalog> LoadCatalog() {
    auto empty = std::make_shared<const Catalog>();
    const std::string path = std::string(kRomDir) + "/catalog.tsv";
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return empty;
    Str text;
    char buffer[1024];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0 && text.size() < kMaxCatalogBytes)
        text.append(buffer, n);
    std::fclose(f);
    auto catalog = std::allocate_shared<Catalog>(tab5_podcast::PsramAllocator<Catalog>(),
                                                 ParseCatalog({text.data(), text.size()}));
    ESP_LOGI(TAG, "catalog: %u games", unsigned(catalog->size()));
    return catalog;
}

std::string Launch(const Game& game) {
    const esp_partition_t* app =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    if (!MdAppPresent(app))
        return "MD 模拟器需 1.2 或更新版本，请用 USB 更新升级程序";
    const std::string rom = std::string(kRomDir) + "/" + std::string(game.file.data(), game.file.size());
    struct stat st;
    if (stat(rom.c_str(), &st) != 0 || st.st_size <= 0)
        return "找不到游戏文件：" + std::string(game.file.data(), game.file.size());
    mkdir("/sdcard/tab5", 0775);
    mkdir(kLaunchDir, 0775);
    FILE* f = std::fopen(kLaunchFile, "w");
    if (!f)
        return "SD 卡无法写入";
    std::fprintf(f, "rom=%s\n", rom.c_str());
    const bool written = std::fflush(f) == 0;
    std::fclose(f);
    if (!written)
        return "SD 卡无法写入";
    if (esp_ota_set_boot_partition(app) != ESP_OK) {
        std::remove(kLaunchFile);
        return "无法切换到 MD 模式";
    }
    ESP_LOGI(TAG, "launching %s in the ota_0 app", rom.c_str());
    tab5_diag::MarkLeavingForOtherApp();
    Application::GetInstance().Reboot();
    return {};
}

}  // namespace tab5_md
