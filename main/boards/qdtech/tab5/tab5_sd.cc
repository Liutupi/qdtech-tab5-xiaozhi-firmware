#include "tab5_sd.h"

#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/sdmmc_host.h>
#include <sd_pwr_ctrl_by_on_chip_ldo.h>
#include <sys/stat.h>
#include <stdio.h>

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#define TAG "Tab5Sd"
#define TAB5_SD_MOUNT "/sdcard"

// M5Stack Tab5 microSD: SDMMC 4-bit on slot 0.
// Slot 1 is occupied by C6 Wi-Fi over SDIO (esp_hosted) — never use it.
// SD card I/O power is LDO_VO4 (must be enabled before card init, else CMD0
// times out with ESP_ERR_TIMEOUT).
#define TAB5_SD_LDO_CHAN 4

// ESP32-P4 has only ONE SDMMC host controller (SDMMC_LL_HOST_CTLR_NUMS == 1).
// ESP-Hosted (C6 Wi-Fi over SDIO) already calls sdmmc_host_init() and claims it.
// A second sdmmc_host_init() from esp_vfs_fat_sdmmc_mount fails with
// ESP_ERR_NOT_FOUND ("no available sd host controller") — ESP-IDF issue 16233.
// Official workaround (esp_hosted/examples/host_sdcard_with_hosted): replace
// host.init/deinit with no-ops so mount only adds slot 0 to the existing controller.
static esp_err_t SdHostInitDummy() {
    return ESP_OK;
}
static esp_err_t SdHostDeinitDummy() {
    return ESP_OK;
}

static sdmmc_card_t* s_card = nullptr;
static sd_pwr_ctrl_handle_t s_pwr = nullptr;
static bool s_ready = false;
static lv_fs_drv_t s_fs_drv;

static void* FsOpen(lv_fs_drv_t* drv, const char* path, lv_fs_mode_t mode) {
    (void)drv;
    char full[256];
    snprintf(full, sizeof(full), "%s/%s", TAB5_SD_MOUNT, path);
    const char* m = (mode & LV_FS_MODE_WR) ? "wb" : "rb";
    return fopen(full, m);
}

static lv_fs_res_t FsClose(lv_fs_drv_t* drv, void* file) {
    (void)drv;
    fclose(static_cast<FILE*>(file));
    return LV_FS_RES_OK;
}

static lv_fs_res_t FsRead(lv_fs_drv_t* drv, void* file, void* buf, uint32_t btr, uint32_t* br) {
    (void)drv;
    *br = fread(buf, 1, btr, static_cast<FILE*>(file));
    return LV_FS_RES_OK;
}

static lv_fs_res_t FsSeek(lv_fs_drv_t* drv, void* file, uint32_t pos, lv_fs_whence_t whence) {
    (void)drv;
    int w = SEEK_SET;
    if (whence == LV_FS_SEEK_CUR) w = SEEK_CUR;
    else if (whence == LV_FS_SEEK_END) w = SEEK_END;
    return fseek(static_cast<FILE*>(file), pos, w) == 0 ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}

static lv_fs_res_t FsTell(lv_fs_drv_t* drv, void* file, uint32_t* pos) {
    (void)drv;
    *pos = ftell(static_cast<FILE*>(file));
    return LV_FS_RES_OK;
}

static void RegisterLvglFs() {
    lv_fs_drv_init(&s_fs_drv);
    s_fs_drv.letter = 'S';
    s_fs_drv.cache_size = 0;
    s_fs_drv.open_cb = FsOpen;
    s_fs_drv.close_cb = FsClose;
    s_fs_drv.read_cb = FsRead;
    s_fs_drv.seek_cb = FsSeek;
    s_fs_drv.tell_cb = FsTell;
    lv_fs_drv_register(&s_fs_drv);
}

bool Tab5SdMountAndRegisterFs() {
    if (s_ready) {
        return true;
    }

    // Match M5Tab5-UserDemo bsp_sdcard_init() + host_sdcard_with_hosted:
    // SDMMC 4-bit, slot 0, LDO_VO4 supplies SDMMC IO / card power.
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    // Reuse the controller ESP-Hosted already created (see comment above).
    host.init = SdHostInitDummy;
    host.deinit = SdHostDeinitDummy;

    if (s_pwr == nullptr) {
        sd_pwr_ctrl_ldo_config_t ldo_config = {
            .ldo_chan_id = TAB5_SD_LDO_CHAN,
        };
        esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_pwr);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "LDO_VO4 (chan %d) power ctrl failed: %s", TAB5_SD_LDO_CHAN,
                     esp_err_to_name(err));
            return false;
        }
        ESP_LOGI(TAG, "SD IO power on LDO_VO%d (3.3V)", TAB5_SD_LDO_CHAN);
    }
    host.pwr_ctrl_handle = s_pwr;

    // P4 defaults already map CLK=43 CMD=44 D0-D3=39-42; set width + pins
    // explicitly so a future IDF default change cannot silently break us.
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = GPIO_NUM_43;
    slot_config.cmd = GPIO_NUM_44;
    slot_config.d0 = GPIO_NUM_39;
    slot_config.d1 = GPIO_NUM_40;
    slot_config.d2 = GPIO_NUM_41;
    slot_config.d3 = GPIO_NUM_42;
    slot_config.cd = SDMMC_SLOT_NO_CD;
    slot_config.wp = SDMMC_SLOT_NO_WP;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    ESP_LOGI(TAG, "Mounting SDMMC 4-bit slot0 at %s (CLK=%d CMD=%d D0-3=%d-%d)", TAB5_SD_MOUNT,
             (int)slot_config.clk, (int)slot_config.cmd, (int)slot_config.d0, (int)slot_config.d3);

    esp_err_t err = esp_vfs_fat_sdmmc_mount(TAB5_SD_MOUNT, &host, &slot_config, &mount_config,
                                            &s_card);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGE(TAG, "FAT mount failed (no partition / wrong format)");
        } else {
            ESP_LOGE(TAG, "SD card init failed (%s) — check card seated and LDO_VO4",
                     esp_err_to_name(err));
        }
        return false;
    }

    sdmmc_card_print_info(stdout, s_card);

    RegisterLvglFs();
    s_ready = true;
    ESP_LOGI(TAG, "SD mounted at %s, LVGL FS 'S' ready", TAB5_SD_MOUNT);
    return true;
}

bool Tab5SdReady() {
    return s_ready;
}
