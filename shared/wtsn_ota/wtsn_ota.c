#include "wtsn_ota.h"

#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

static const char *TAG = "wtsn_ota";

#define OTA_TASK_STACK 8192

static volatile int g_busy = 0;

typedef struct {
    char *url;
    char *crc;   /* hex CRC32 string, or NULL to skip verification */
} ota_job_t;

/* Re-read the image the OTA layer just wrote (the non-running update
 * partition, now PENDING_VERIFY) and compare its CRC32 with the value the
 * GUI computed at upload time. Only the actual image bytes are covered, not
 * the whole partition: the ESP image header stores the total image size at
 * byte offset 8 (little-endian), matching the .bin file the GUI hashed.
 * Standard zlib CRC32: running state seeded with 0xFFFFFFFF, final XOR. */
static esp_err_t ota_verify_crc(const char *crc_hex) {
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "CRC check: no update partition found");
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t hdr[16];
    if (esp_partition_read(part, 0, hdr, sizeof(hdr)) != ESP_OK ||
        hdr[0] != 0xE9) {
        ESP_LOGE(TAG, "CRC check: no valid ESP image header (magic=0x%02X)",
                 hdr[0]);
        return ESP_FAIL;
    }
    uint32_t img_size = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) |
                        ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
    if (img_size == 0 || img_size > part->size) {
        ESP_LOGE(TAG, "CRC check: implausible image size %u (part %u)",
                 (unsigned)img_size, (unsigned)part->size);
        return ESP_FAIL;
    }
    uint32_t expect = (uint32_t)strtoul(crc_hex, NULL, 16);
    uint8_t buf[4096];
    size_t off = 0;
    uint32_t state = 0xFFFFFFFFu;
    while (off < img_size) {
        size_t n = img_size - off < sizeof(buf) ? img_size - off : sizeof(buf);
        if (esp_partition_read(part, off, buf, n) != ESP_OK) {
            ESP_LOGE(TAG, "CRC check: partition read failed at offset %u",
                     (unsigned)off);
            return ESP_FAIL;
        }
        state = esp_rom_crc32_le(state, buf, n);
        off += n;
    }
    uint32_t got = state ^ 0xFFFFFFFFu;
    if (got != expect) {
        ESP_LOGE(TAG, "CRC32 mismatch: got %08lX expect %08lX - image "
                 "corrupted, aborting (old app stays active)",
                 (unsigned long)got, (unsigned long)expect);
        /* esp_https_ota() already pointed the boot partition at the new
         * image; point it back at the known-good running app so a later
         * reboot cannot land on the corrupted one. */
        const esp_partition_t *run = esp_ota_get_running_partition();
        if (run) esp_ota_set_boot_partition(run);
        return ESP_ERR_INVALID_CRC;
    }
    ESP_LOGI(TAG, "CRC32 verified: %08lX (%u bytes)", (unsigned long)got,
             (unsigned)img_size);
    return ESP_OK;
}

static void ota_task(void *arg) {
    ota_job_t *job = (ota_job_t *)arg;
    char *url = job->url;
    esp_err_t err = ESP_FAIL;

    ESP_LOGI(TAG, "OTA update from %s", url);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 30000,
    };
    esp_https_ota_config_t cfg = {
        .http_config = &http_cfg,
    };

    /* Correct API: begin() allocates the handle, then perform/finish stream the
     * image onto the other partition. Passing a pointer to the *config* struct
     * as the handle (the previous code) relies on unspecified struct layout
     * and leaks/use-after-free as soon as IDF reshuffles it. */
    esp_https_ota_handle_t handle = NULL;
    err = esp_https_ota_begin(&cfg, &handle);
    if (err == ESP_OK) {
        err = esp_https_ota_perform(handle);
        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) err = ESP_OK;
    }
    if (err == ESP_OK) {
        esp_err_t ferr = esp_https_ota_finish(handle);
        if (ferr != ESP_OK) {
            ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(ferr));
            err = ferr;
        } else if (job->crc) {
            err = ota_verify_crc(job->crc);
        }
    } else {
        ESP_LOGE(TAG, "OTA failed: %s (previous app stays active)", esp_err_to_name(err));
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA download complete, rebooting into new app");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }

    free(url);
    free(job->crc);
    free(job);
    g_busy = 0;
    vTaskDelete(NULL);
}

esp_err_t wtsn_ota_start_checked(const char *url, const char *crc32_hex) {
    if (!url || !url[0]) return ESP_ERR_INVALID_ARG;
    if (g_busy) {
        ESP_LOGW(TAG, "OTA already in progress, ignoring");
        return ESP_ERR_INVALID_STATE;
    }
    ota_job_t *job = calloc(1, sizeof(*job));
    if (!job) return ESP_ERR_NO_MEM;
    job->url = strdup(url);
    job->crc = crc32_hex ? strdup(crc32_hex) : NULL;
    if (!job->url || (crc32_hex && !job->crc)) {
        free(job->url);
        free(job->crc);
        free(job);
        return ESP_ERR_NO_MEM;
    }
    g_busy = 1;
    if (xTaskCreate(ota_task, "wtsn_ota", OTA_TASK_STACK, job, 4, NULL) != pdPASS) {
        free(job->url);
        free(job->crc);
        free(job);
        g_busy = 0;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t wtsn_ota_start(const char *url) {
    return wtsn_ota_start_checked(url, NULL);
}

esp_err_t wtsn_ota_start_sized(const char *url, size_t size) {
    (void)size;   /* size is informational; the download verifies its own length */
    return wtsn_ota_start(url);
}
