idf_component_register(
    SRCS "src/ota_update.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES app_update esp_app_format
)
