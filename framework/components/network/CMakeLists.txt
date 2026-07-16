# network: Wi-Fi station bring-up. (Later: RAOP RTSP/RTP transport plugin.)
idf_component_register(
    SRCS "src/wifi.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_wifi esp_event esp_netif esp_timer system
)
