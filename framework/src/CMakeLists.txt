# Registers src/ as the project's "main" component.
# PlatformIO's ESP-IDF builder and a raw `idf.py build` both honor this.
idf_component_register(
    SRCS "main.cpp" "udp_log.cpp"
    INCLUDE_DIRS "."
)
