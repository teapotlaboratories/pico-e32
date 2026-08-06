# wifi_manager: one STA front-end (wifi_manager.c) over the esp_wifi API, on both boards. The API is identical;
# only the backend differs by target, so the REQUIRES is gated on IDF_TARGET (NOT CONFIG_*, which isn't defined
# during the early requirement-expansion pass — a CONFIG_-gated REQUIRES silently drops esp_wifi's includes):
#   * ESP32-S3  -> native esp_wifi radio.
#   * ESP32-P4  -> no radio; esp_wifi_remote forwards the same calls over esp-hosted (SDIO) to the ESP32-C6
#                  companion. The managed deps (esp_wifi_remote/esp_hosted) come from the app's idf_component.yml
#                  (P4-only rule); esp_wifi here supplies the API headers.
idf_build_get_property(target IDF_TARGET)
if(target STREQUAL "esp32p4")
    idf_component_register(
        SRCS "wifi_manager.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_wifi esp_wifi_remote esp_hosted esp_netif esp_event nvs_flash
    )
    message(STATUS "wifi: esp_wifi_remote -> ESP32-C6 over esp-hosted (${target})")
else()
    idf_component_register(
        SRCS "wifi_manager.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_wifi esp_netif esp_event nvs_flash
    )
    message(STATUS "wifi: native esp_wifi STA (${target})")
endif()
