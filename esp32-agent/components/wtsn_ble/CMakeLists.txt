idf_component_register(
    SRCS "wtsn_ble.c"
    INCLUDE_DIRS "."
    REQUIRES bt wtsn_agent esp_timer nvs_flash driver
)
