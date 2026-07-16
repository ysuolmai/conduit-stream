# audio: transport-agnostic Playback Manager, PCM ring buffer, I2S wrapper.
idf_component_register(
    SRCS "src/audio.c"
         "src/audio_i2s.c"
         "src/audio_ringbuf.c"
         "src/audio_playback.c"
         "src/audio_drift.c"
         "src/audio_diag.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    PRIV_REQUIRES driver esp_driver_i2s esp_psram
)
