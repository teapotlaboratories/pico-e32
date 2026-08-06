# ota_manager: firmware update over WiFi (WC-4a). Board-agnostic — it writes flash, never storage, and never
# touches the radio itself; the caller holds a wifi_mgr_acquire() around it.
idf_component_register(
    SRCS "ota_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_http_client esp-tls mbedtls   # mbedtls provides PSA Crypto (see ota_manager.c)
)
