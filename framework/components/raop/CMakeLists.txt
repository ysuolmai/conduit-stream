# raop: the AirPlay-1 (RAOP) protocol plugin — RTSP control server, RSA challenge,
# AES session-key exchange, UDP socket bind, and (Phase 3) the RTP audio
# receive + AES-128-CBC decrypt + ALAC decode -> audio_play_pcm() path.
#
# Public raop.h exposes only void functions, so every dependency is private:
#   mbedtls        — PSA Crypto + mbedtls_pk_parse_key (challenge sign, AES-key
#                    decrypt, AES-128-CBC audio decrypt)
#   lwip           — BSD sockets (RTSP TCP server + UDP bind + RTP recvfrom)
#   esp_hw_support — esp_read_mac / esp_mac.h (STA MAC for the challenge buffer)
#   discovery      — RAOP_RTSP_PORT from mdns_service.h
#   alac           — vendored ALAC decoder + pure fmtp->config mapper (Phase 3)
#   audio          — audio_play_pcm() + single-producer diag-tone handoff (Phase 3)
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
         "src/rtp_parser.c"
         "src/aes_frame.c"
         "src/rtp_reorder.c"
         "src/rtp_resend.c"
         "src/rtp_timing.c"
         "src/raop_rtp.c"
         "src/raop_volume.c"
         "src/dmap.c"
         "src/setparam.c"
         "src/raop_metadata.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    PRIV_REQUIRES mbedtls lwip esp_hw_support esp_timer discovery alac audio
)
