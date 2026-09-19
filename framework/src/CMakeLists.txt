# Registers src/ as the project's "main" component.
# PlatformIO's ESP-IDF builder and a raw `idf.py build` both honor this.
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        audio
        discovery
        esp_psram
        esp_system
        freertos
        log
        network
        nvs_flash
        raop
        spi_flash
        system
)
