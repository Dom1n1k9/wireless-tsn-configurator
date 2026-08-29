#include "wtsn_ble.h"
#include "wtsn_sensor.h"

#include <string.h>
#include <stdio.h>
#include <stddef.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_bt.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";

/* Discovered device flag for scan-by-name. */
static bool g_found_mb = false;

/* Nordic UART Service (NUS): service 0001, rx (write) 0002. */
static const ble_uuid128_t uart_svc_uuid = {
    .u = { .type = BLE_UUID_TYPE_128 },
    .value = { 0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x01,0x00,0x40,0x6e },
};
static const ble_uuid128_t uart_rx_uuid = {
    .u = { .type = BLE_UUID_TYPE_128 },
    .value = { 0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x02,0x00,0x40,0x6e },
};

static uint16_t g_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t g_rx_handle = 0;

static int gap_cb(struct ble_gap_event *event, void *arg);

static int start_scan(void) {
    struct ble_gap_disc_params params;
    memset(&params, 0, sizeof(params));
    params.filter_duplicates = 1;
    params.passive = 1;
    params.limited = 0;
    uint8_t own_addr;
    if (ble_hs_id_infer_auto(0, &own_addr) != 0) return -1;
    int rc = ble_gap_disc(own_addr, BLE_HS_FOREVER, &params, gap_cb, NULL);
    if (rc != 0) ESP_LOGW(TAG, "scan rc=%d", rc);
    return rc;
}

static int disc_chrs_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg) {
    (void)conn_handle; (void)error; (void)chr; (void)arg;
    if (!chr) { ESP_LOGW(TAG, "no NUS char found"); return 0; }
    ESP_LOGI(TAG, "  chr handle=%x uuidtype=%d",
             chr->val_handle, chr->uuid.u.type);
    if (chr->uuid.u.type == BLE_UUID_TYPE_128 &&
        ble_uuid_cmp(&chr->uuid.u, &uart_rx_uuid.u) == 0) {
        g_rx_handle = chr->val_handle;
        ESP_LOGI(TAG, "NUS RX char found, handle=%x", g_rx_handle);
    }
    return 0;
}

static int disc_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                      const struct ble_gatt_svc *service, void *arg) {
    (void)conn_handle; (void)error; (void)arg;
    if (!service) { ESP_LOGW(TAG, "svc discovery done: err=%d", error ? error->status : 0); return 0; }
    ESP_LOGI(TAG, "  svc start=0x%x end=0x%x", service->start_handle, service->end_handle);
    ble_gattc_disc_chrs_by_uuid(conn_handle, service->start_handle,
                                 service->end_handle, &uart_rx_uuid.u,
                                 disc_chrs_cb, NULL);
    return 0;
}

static int gap_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_gap_disc_desc *d = &event->disc;
        /* Look for a micro:bit in the advertisement data (device name) and
         * connect to the first one found. Enables scanning by name instead of a
         * hard-coded MAC (the micro:bit's random BLE address changes often). */
        if (!g_found_mb) {
            struct ble_hs_adv_fields adv;
            int rc = ble_hs_adv_parse_fields(&adv, d->data, d->length_data);
            static int scan_cnt = 0;
            scan_cnt++;
            ESP_LOGI(TAG, "scan#%d rssi=%d len=%d  (%02x:%02x:%02x:%02x:%02x:%02x)%s",
                     scan_cnt, d->rssi, d->length_data,
                     d->addr.val[0], d->addr.val[1], d->addr.val[2],
                     d->addr.val[3], d->addr.val[4], d->addr.val[5],
                     rc == 0 && adv.name ? "  name=" : "");
            if (rc == 0 && adv.name)
                ESP_LOGI(TAG, "    name: %.*s", adv.name_len, adv.name);
            static const uint8_t mb_mac[6] = {0xD1,0xE8,0xF4,0xEA,0x04,0x00};
            bool name_mb = rc == 0 && adv.name && adv.name_len >= 3 &&
                           (strncmp((char *)adv.name, "micro", 5) == 0 ||
                            strncmp((char *)adv.name, "BBC", 3) == 0);
            bool mac_mb = memcmp(d->addr.val, mb_mac, 6) == 0;
            if (name_mb || mac_mb) {
                ESP_LOGI(TAG, "advertised: %s (%02x:%02x:%02x:%02x:%02x:%02x)",
                         name_mb ? (char *)adv.name : "<mac-match>",
                         d->addr.val[0], d->addr.val[1], d->addr.val[2],
                         d->addr.val[3], d->addr.val[4], d->addr.val[5]);
                g_found_mb = true;
                ble_gap_disc_cancel();
                uint8_t own_addr;
                if (ble_hs_id_infer_auto(0, &own_addr) != 0) return 0;
                int c = ble_gap_connect(own_addr, &d->addr, 30000, NULL, gap_cb, NULL);
                if (c != 0) { g_found_mb = false; vTaskDelay(pdMS_TO_TICKS(1000)); start_scan(); }
            }
        }
        return 0;
    }
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            g_conn = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected, discovering NUS...");
            ble_gattc_disc_svc_by_uuid(g_conn, &uart_svc_uuid.u, disc_svc_cb, NULL);
        } else {
            vTaskDelay(pdMS_TO_TICKS(3000));
            start_scan();
        }
        return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        vTaskDelay(pdMS_TO_TICKS(3000));
        start_scan();
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        g_conn = BLE_HS_CONN_HANDLE_NONE;
        g_rx_handle = 0;
        g_found_mb = false;     /* re-scan: micro:bit random addr may have changed */
        vTaskDelay(pdMS_TO_TICKS(3000));
        start_scan();
        return 0;
    default:
        return 0;
    }
}

static void send_to_panel(void) {
    if (g_conn == BLE_HS_CONN_HANDLE_NONE || g_rx_handle == 0) return;
    float temp = 0, hum = 0;
    int light = 0, pir = 0, actor = 0;
    wtsn_sensor_last(&temp, &hum, &light, &pir, &actor);
    char line[96];
    int n = snprintf(line, sizeof(line), "T:%.1f H:%.1f L:%d P:%d", temp, hum, light, pir);
    int rc = ble_gattc_write_flat(g_conn, g_rx_handle, (void *)line, (uint16_t)n, NULL, NULL);
    if (rc != 0) ESP_LOGW(TAG, "write rc=%d", rc);
}

static void ble_task(void *arg) {
    (void)arg;
    while (1) {
        if (g_conn != BLE_HS_CONN_HANDLE_NONE && g_rx_handle != 0) send_to_panel();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void on_reset(int reason) { ESP_LOGE(TAG, "host reset reason=%d", reason); }

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    ESP_LOGI(TAG, "host synced");
    start_scan();
}

static void host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void wtsn_ble_start(void) {
    nvs_flash_init();
    if (nimble_port_init() != ESP_OK) { ESP_LOGE(TAG, "nimble init failed"); return; }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_device_name_set("wtsn-esp");

    xTaskCreatePinnedToCore(ble_task, "ble_panel", 4096, NULL, 6, NULL, 1);
    nimble_port_freertos_init(host_task);
}

void wtsn_ble_restart(void) {
    if (g_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(g_conn, 0x13);
}
