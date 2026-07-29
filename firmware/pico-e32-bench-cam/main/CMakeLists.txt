# cam_main.c uses driver/gpio.h + driver/ledc.h (split out to esp_driver_* in ESP-IDF v6) and the LEGACY
# driver/i2c.h for the camera's SCCB (still shipped by the `driver` component in v6, so keep it). The specific
# esp_driver_* components exist on v5.3+, so this stays valid on v5.4.2.
idf_component_register(
    SRCS "cam_main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_http_server nvs_flash esp_netif esp_event driver esp_driver_gpio esp_driver_ledc
)

# WiFi credentials are passed on the build command line and never stored in the tree:
#   make flash APP=pico-e32-bench-cam BOARD=m5stack-timer-cam PORT=/dev/ttyUSB0 \
#        WIFI_SSID='my ssid' WIFI_PASS='my pass'
if(DEFINED WIFI_SSID AND DEFINED WIFI_PASS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        WIFI_SSID="${WIFI_SSID}" WIFI_PASS="${WIFI_PASS}")
endif()
