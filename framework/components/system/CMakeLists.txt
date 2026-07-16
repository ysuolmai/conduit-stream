# system: NVS config store, device identity. (Later: LED, health, RAOP RSA key.)
idf_component_register(
    SRCS "src/system_config.c"
         "src/device_id.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    PRIV_REQUIRES nvs_flash esp_hw_support
)
