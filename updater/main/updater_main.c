// Tab5 SD-card firmware updater.
//
// Runs from the small `ota_0` slot. The main firmware (in `factory`) downloads a release to
// /sdcard/ota/tab5-update.bin plus a tab5-update.txt (sha256=, size=, version=), verifies the
// SHA-256 and reboots here. This program re-verifies the file, erases and rewrites `factory`,
// reads the flash back and checks the SHA-256 again, and only then points the bootloader back
// to `factory` (esp_ota_set_boot_partition on a factory image erases otadata).
//
// Power loss at any point is safe: otadata keeps pointing at this updater and the SD files are
// only removed after a fully verified write, so the next boot simply retries.
//
// The same app also hosts MD (Mega Drive) games (md_main.c): with no firmware update pending,
// a launch request on the SD card runs the Gwenesis emulator with the whole chip to itself.
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "md_main.h"
#include "psa/crypto.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

static const char* TAG = "tab5_updater";

#define SD_MOUNT "/sdcard"
#define UPDATE_BIN SD_MOUNT "/ota/tab5-update.bin"
#define UPDATE_META SD_MOUNT "/ota/tab5-update.txt"
#define BAD_BIN SD_MOUNT "/ota/tab5-update.bad"
#define BAD_META SD_MOUNT "/ota/tab5-update.txt.bad"
#define SD_LDO_CHAN 4
#define CHUNK_SIZE (64 * 1024)
#define MIN_IMAGE_SIZE (256 * 1024)
#define WRITE_ATTEMPTS 3

static sdmmc_card_t* s_card = NULL;
static sd_pwr_ctrl_handle_t s_pwr = NULL;

static void delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static bool sd_mount(void) {
    if (s_pwr == NULL) {
        sd_pwr_ctrl_ldo_config_t ldo = {.ldo_chan_id = SD_LDO_CHAN};
        esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SD LDO init failed: %s", esp_err_to_name(err));
            return false;
        }
    }
    for (int attempt = 0; attempt < 4; ++attempt) {
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.slot = SDMMC_HOST_SLOT_0;
        host.max_freq_khz = attempt < 2 ? SDMMC_FREQ_HIGHSPEED : SDMMC_FREQ_DEFAULT;
        host.pwr_ctrl_handle = s_pwr;

        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = 4;
        slot.clk = GPIO_NUM_43;
        slot.cmd = GPIO_NUM_44;
        slot.d0 = GPIO_NUM_39;
        slot.d1 = GPIO_NUM_40;
        slot.d2 = GPIO_NUM_41;
        slot.d3 = GPIO_NUM_42;
        slot.cd = SDMMC_SLOT_NO_CD;
        slot.wp = SDMMC_SLOT_NO_WP;
        slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

        esp_vfs_fat_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 4,
            .allocation_unit_size = 16 * 1024,
        };
        esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &mount_config, &s_card);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "SD mounted (attempt %d)", attempt + 1);
            return true;
        }
        ESP_LOGW(TAG, "SD mount attempt %d failed: %s", attempt + 1, esp_err_to_name(err));
        delay_ms(1000);
    }
    return false;
}

static void to_hex(const uint8_t digest[32], char out[65]) {
    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out[i * 2] = kHex[digest[i] >> 4];
        out[i * 2 + 1] = kHex[digest[i] & 0x0f];
    }
    out[64] = '\0';
}

static bool finish_hash(psa_hash_operation_t* op, char out_hex[65]) {
    uint8_t digest[32];
    size_t len = 0;
    if (psa_hash_finish(op, digest, sizeof(digest), &len) != PSA_SUCCESS || len != sizeof(digest)) {
        psa_hash_abort(op);
        return false;
    }
    to_hex(digest, out_hex);
    return true;
}

// SHA-256 of a whole file.
static bool sha256_of_file(const char* path, uint8_t* buf, char out_hex[65]) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "open %s failed: errno=%d", path, errno);
        return false;
    }
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        fclose(f);
        return false;
    }
    bool ok = true;
    while (true) {
        size_t n = fread(buf, 1, CHUNK_SIZE, f);
        if (n > 0 && psa_hash_update(&op, buf, n) != PSA_SUCCESS) {
            ok = false;
            break;
        }
        if (n < CHUNK_SIZE) {
            ok = ok && !ferror(f);
            break;
        }
    }
    fclose(f);
    if (!ok) {
        psa_hash_abort(&op);
        return false;
    }
    return finish_hash(&op, out_hex);
}

// SHA-256 of the first `size` bytes of a flash partition.
static bool sha256_of_partition(const esp_partition_t* part, uint32_t size, uint8_t* buf,
                                char out_hex[65]) {
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return false;
    }
    uint32_t offset = 0;
    while (offset < size) {
        uint32_t n = size - offset < CHUNK_SIZE ? size - offset : CHUNK_SIZE;
        if (esp_partition_read(part, offset, buf, n) != ESP_OK ||
            psa_hash_update(&op, buf, n) != PSA_SUCCESS) {
            psa_hash_abort(&op);
            return false;
        }
        offset += n;
    }
    return finish_hash(&op, out_hex);
}

static bool is_hex64(const char* s) {
    for (int i = 0; i < 64; ++i) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return s[64] == '\0' || s[64] == '\r' || s[64] == '\n';
}

static bool read_meta(const char* path, char sha_out[65], uint32_t* size_out, char version_out[32]) {
    FILE* f = fopen(path, "r");
    if (!f) {
        return false;
    }
    char line[160];
    bool have_sha = false, have_size = false;
    version_out[0] = '\0';
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "sha256=", 7) == 0 && is_hex64(line + 7)) {
            for (int i = 0; i < 64; ++i) {
                char c = line[7 + i];
                sha_out[i] = (c >= 'A' && c <= 'F') ? (char)(c - 'A' + 'a') : c;
            }
            sha_out[64] = '\0';
            have_sha = true;
        } else if (strncmp(line, "size=", 5) == 0) {
            *size_out = (uint32_t)strtoul(line + 5, NULL, 10);
            have_size = true;
        } else if (strncmp(line, "version=", 8) == 0) {
            strncpy(version_out, line + 8, 31);
            version_out[31] = '\0';
            char* nl = strpbrk(version_out, "\r\n");
            if (nl) *nl = '\0';
        }
    }
    fclose(f);
    return have_sha && have_size;
}

static void quarantine_update(void) {
    unlink(BAD_BIN);
    unlink(BAD_META);
    rename(UPDATE_BIN, BAD_BIN);
    rename(UPDATE_META, BAD_META);
}

// Cheap sanity check that the file is an ESP32-P4 image of the Tab5 firmware.
static bool check_image_header(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    uint8_t head[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)];
    size_t n = fread(head, 1, sizeof(head), f);
    fclose(f);
    if (n != sizeof(head)) {
        return false;
    }
    esp_image_header_t hdr;
    memcpy(&hdr, head, sizeof(hdr));
    esp_app_desc_t desc;
    memcpy(&desc, head + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(desc));
    if (hdr.magic != ESP_IMAGE_HEADER_MAGIC || hdr.chip_id != ESP_CHIP_ID_ESP32P4) {
        ESP_LOGE(TAG, "not an ESP32-P4 image (magic=0x%02x chip=0x%x)", hdr.magic, (unsigned)hdr.chip_id);
        return false;
    }
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD || strncmp(desc.project_name, "xiaozhi", 7) != 0) {
        ESP_LOGE(TAG, "unexpected app descriptor (project=%.32s)", desc.project_name);
        return false;
    }
    ESP_LOGI(TAG, "image ok: project=%.32s version=%.32s", desc.project_name, desc.version);
    return true;
}

static bool write_image(const esp_partition_t* dst, const char* path, uint32_t size, const char* want_sha,
                        uint8_t* buf) {
    const uint32_t erase_len = (size + 0xfffu) & ~0xfffu;
    ESP_LOGI(TAG, "erasing %u bytes of '%s'", (unsigned)erase_len, dst->label);
    for (uint32_t off = 0; off < erase_len;) {
        uint32_t step = erase_len - off < 0x10000 ? erase_len - off : 0x10000;
        if (esp_partition_erase_range(dst, off, step) != ESP_OK) {
            ESP_LOGE(TAG, "erase failed at 0x%x", (unsigned)off);
            return false;
        }
        off += step;
    }

    FILE* f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    uint32_t offset = 0;
    int last_pct = -1;
    bool ok = true;
    while (offset < size) {
        uint32_t want = size - offset < CHUNK_SIZE ? size - offset : CHUNK_SIZE;
        size_t n = fread(buf, 1, want, f);
        if (n != want) {
            ESP_LOGE(TAG, "short read at %u", (unsigned)offset);
            ok = false;
            break;
        }
        if (esp_partition_write(dst, offset, buf, n) != ESP_OK) {
            ESP_LOGE(TAG, "flash write failed at 0x%x", (unsigned)offset);
            ok = false;
            break;
        }
        offset += (uint32_t)n;
        int pct = (int)((uint64_t)offset * 100 / size);
        if (pct / 10 != last_pct / 10) {
            ESP_LOGI(TAG, "writing %d%%", pct);
            last_pct = pct;
        }
    }
    fclose(f);
    if (!ok) {
        return false;
    }

    ESP_LOGI(TAG, "verifying flash contents");
    char got[65];
    if (!sha256_of_partition(dst, size, buf, got) || strcmp(got, want_sha) != 0) {
        ESP_LOGE(TAG, "flash verify failed (got %s)", got);
        return false;
    }
    return true;
}

// Returns true when `factory` now holds the new image, or when there was nothing to apply.
// Sets *retry_later when the SD update must be kept for another attempt.
static bool apply_update(const esp_partition_t* factory, uint8_t* buf, bool* retry_later) {
    *retry_later = false;
    char want_sha[65];
    char version[32];
    uint32_t size = 0;
    if (!read_meta(UPDATE_META, want_sha, &size, version)) {
        ESP_LOGW(TAG, "no valid update metadata on SD");
        return true;
    }
    struct stat st;
    if (stat(UPDATE_BIN, &st) != 0 || (uint32_t)st.st_size != size || size < MIN_IMAGE_SIZE ||
        size > factory->size) {
        ESP_LOGE(TAG, "update file missing or wrong size (meta=%u)", (unsigned)size);
        quarantine_update();
        return true;
    }
    ESP_LOGI(TAG, "applying update version=%s size=%u", version, (unsigned)size);
    if (!check_image_header(UPDATE_BIN)) {
        quarantine_update();
        return true;
    }
    char got[65];
    if (!sha256_of_file(UPDATE_BIN, buf, got) || strcmp(got, want_sha) != 0) {
        ESP_LOGE(TAG, "SD file SHA-256 mismatch (got %s)", got);
        quarantine_update();
        return true;
    }
    for (int attempt = 1; attempt <= WRITE_ATTEMPTS; ++attempt) {
        ESP_LOGI(TAG, "write attempt %d/%d", attempt, WRITE_ATTEMPTS);
        if (write_image(factory, UPDATE_BIN, size, want_sha, buf)) {
            unlink(UPDATE_BIN);
            unlink(UPDATE_META);
            ESP_LOGI(TAG, "update written and verified");
            return true;
        }
    }
    *retry_later = true;
    return false;
}

static void boot_factory_forever(const esp_partition_t* factory) {
    while (true) {
        // For a factory image this validates the image and erases otadata.
        esp_err_t err = esp_ota_set_boot_partition(factory);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "booting main firmware");
            delay_ms(300);
            esp_restart();
        }
        ESP_LOGE(TAG, "main firmware is not bootable (%s). Reflash over USB.", esp_err_to_name(err));
        delay_ms(5000);
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Tab5 updater 1.0");
    // Keep the bootloader's rollback logic from ever treating this slot as a failed update.
    esp_ota_mark_app_valid_cancel_rollback();

    const esp_partition_t* factory =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory == NULL) {
        ESP_LOGE(TAG, "no factory partition");
        while (true) delay_ms(5000);
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed");
        boot_factory_forever(factory);
    }
    uint8_t* buf = heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(TAG, "no memory");
        boot_factory_forever(factory);
    }

    if (sd_mount()) {
        bool retry_later = false;
        if (!apply_update(factory, buf, &retry_later) && retry_later) {
            ESP_LOGE(TAG, "update failed; SD files kept, retrying in 30 s");
            delay_ms(30000);
            esp_restart();
        }
        free(buf);
        md_maybe_run(factory);  // returns at once without an MD launch request
    } else {
        ESP_LOGW(TAG, "SD card not available");
    }
    boot_factory_forever(factory);
}
