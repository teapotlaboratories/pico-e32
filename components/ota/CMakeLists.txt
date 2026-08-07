# ota_manager: firmware update over WiFi (WC-4a). Board-agnostic — it writes flash, never storage, and never
# touches the radio itself; the caller holds a wifi_mgr_acquire() around it.
idf_component_register(
    SRCS "ota_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_http_client esp-tls mbedtls   # mbedtls provides PSA Crypto (see ota_manager.c)
)

# `make ... DEFS='-D OTA_INSECURE=1'` arrives as a CMake cache variable, NOT a compile definition — each flag
# has to be forwarded per component, per name. Without this the #if in ota_manager.c is dead: a bench build
# against a self-signed server would still validate against the CA bundle, fail the handshake, and never print
# the "never ship this" warning. Same trap as the manifest URL (which is Kconfig for that reason).
if(DEFINED OTA_INSECURE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE OTA_INSECURE=${OTA_INSECURE})
    message(WARNING "ota: OTA_INSECURE=${OTA_INSECURE} — TLS certificates will NOT be verified. Bench only.")
endif()
