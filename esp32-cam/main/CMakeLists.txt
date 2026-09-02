idf_component_register(
    SRCS "camera_app.c"
    INCLUDE_DIRS "."
    REQUIRES esp32-camera esp_wifi esp_netif esp_event esp_http_server mqtt nvs_flash esp_timer driver esp_adc wtsn_prov wtsn_version
)
