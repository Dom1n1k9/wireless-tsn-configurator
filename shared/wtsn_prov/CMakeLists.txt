idf_component_register(
    SRCS "wtsn_prov.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event esp_http_server nvs esp_log
)
