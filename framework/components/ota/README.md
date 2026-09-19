# ota

Streaming OTA writer for the N4R2 dual-slot partition table. It validates that
an upload contains an ESP application descriptor, writes it directly to the
inactive app partition, verifies it with `esp_ota_end()`, and changes the boot
partition only after the complete image succeeds.

NVS is a separate partition, so an OTA update preserves the saved Wi-Fi SSID
and password. Web uploads must use the matching `*-ota.bin` release asset, not
the merged image intended for USB flashing at address `0x0`.
