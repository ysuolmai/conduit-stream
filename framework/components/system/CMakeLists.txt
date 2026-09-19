# system: NVS config store, device identity, RGB status LED. (Later: health, RSA key.)
# led_strip is the espressif/led_strip managed component (idf_component.yml); after
# editing idf_component.yml or REQUIRES, wipe .pio/build/esp32-s3-n16r8 before build.
idf_component_register(
    SRCS "src/system_config.c"
         "src/device_id.c"
         "src/led_state.c"
         "src/setup_button.c"
         "src/system_led.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    PRIV_REQUIRES nvs_flash esp_hw_support driver esp_driver_rmt esp_system
                  freertos led_strip
)
