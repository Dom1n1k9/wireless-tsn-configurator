idf_component_register(
    SRCS "wtsn_tsn.c" "wtsn_mqtt.c" "wtsn_cfg.c" "wtsn_json.c" "wtsn_prov.c" "wtsn_ptp.c" "wtsn_sensor.c" "wtsn_uart.c"
    INCLUDE_DIRS "."
    REQUIRES mqtt nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_timer driver esp_adc
)
