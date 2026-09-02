idf_component_register(
    SRCS "wtsn_ota.c"
    INCLUDE_DIRS "."
    REQUIRES esp_https_ota esp_http_client esp_partition log
)
