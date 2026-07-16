# raop: the AirPlay-1 (RAOP) protocol plugin — RTSP control server, RSA challenge,
# AES session-key exchange, UDP socket bind. (RTP audio receive/decode is Phase 3.)
#
# Public raop.h exposes only void functions, so every dependency is private:
#   mbedtls        — PSA Crypto + mbedtls_pk_parse_key (challenge sign, AES-key decrypt)
#   lwip           — BSD sockets (RTSP TCP server + UDP bind)
#   esp_hw_support — esp_read_mac / esp_mac.h (STA MAC for the challenge buffer)
#   discovery      — RAOP_RTSP_PORT from mdns_service.h
# After editing REQUIRES, wipe .pio/build/esp32-s3-n16r8 before rebuilding.
idf_component_register(
    SRCS "src/raop.c"
         "src/rtsp_parser.c"
         "src/rtsp_session.c"
         "src/raop_crypto.c"
         "src/raop_key.c"
         "src/base64.c"
         "src/sdp.c"
         "src/raop_challenge.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    PRIV_REQUIRES mbedtls lwip esp_hw_support discovery
)
