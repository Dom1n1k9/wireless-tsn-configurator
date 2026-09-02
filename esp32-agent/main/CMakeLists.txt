idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES wtsn_agent wtsn_prov wtsn_version wtsn_ota nvs_flash esp_wifi esp_netif esp_event driver lwip
)

# Optional fixed per-board device id (overrides the runtime auto-detection):
#   idf.py build -D WTSN_DEVICE_ID=esp32-01
# Leave unset to auto-pick (BME280 present -> esp32-01, else esp32-02 relay).
if(DEFINED WTSN_DEVICE_ID)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WTSN_DEVICE_ID="${WTSN_DEVICE_ID}")
endif()
