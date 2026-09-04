#include "wtsn_ota.h"

#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "string.h"
#include "stdio.h"

static const char *TAG = "wtsn_ota";

#define OTA_TASK_STACK 8192

static volatile int g_busy = 0;

static void ota_task(void *arg) {
    char *url = (char *)arg;
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
        } else {
            ESP_LOGI(TAG, "OTA download complete, rebooting into new app");
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
    } else {
        ESP_LOGE(TAG, "OTA failed: %s (previous app stays active)", esp_err_to_name(err));
    }

    free(url);
    g_busy = 0;
    vTaskDelete(NULL);
}

esp_err_t wtsn_ota_start(const char *url) {
    if (!url || !url[0]) return ESP_ERR_INVALID_ARG;
    if (g_busy) {
        ESP_LOGW(TAG, "OTA already in progress, ignoring");
        return ESP_ERR_INVALID_STATE;
    }
    g_busy = 1;
    char *copy = strdup(url);
    if (!copy) { g_busy = 0; return ESP_ERR_NO_MEM; }
    if (xTaskCreate(ota_task, "wtsn_ota", OTA_TASK_STACK, copy, 4, NULL) != pdPASS) {
        free(copy);
        g_busy = 0;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t wtsn_ota_start_sized(const char *url, size_t size) {
    (void)size;   /* size is informational; the download verifies its own length */
    return wtsn_ota_start(url);
}
