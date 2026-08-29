idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES wtsn_agent wtsn_ble nvs_flash esp_wifi esp_netif esp_event driver
)
