# Source / attribution

This app is a copy of the **`host_performs_slave_ota`** example from Espressif's
**esp-hosted** (`espressif/esp-hosted-mcu`, pulled as the managed component `espressif/esp_hosted`
for ESP-IDF v6). It updates the ESP32-C6 co-processor's esp-hosted *slave* firmware over the existing
SDIO link (no extra hardware). Adapted for the Guition JC4880P443C (ESP32-P4 + ESP32-C6): LittleFS
method, the board's P4 PSRAM/flash config, and the freshly-built C6 slave binary at
`components/ota_littlefs/slave_fw_bin/`. Only the config/payload were changed; the OTA logic is upstream's.
