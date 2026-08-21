idf_component_register(
    SRCS "wtsn_agent.c" "wtsn_tsn.c" "wtsn_mqtt.c" "wtsn_cfg.c" "wtsn_json.c"
    INCLUDE_DIRS "."
    REQUIRES mqtt nvs_flash
)
