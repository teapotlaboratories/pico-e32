# app_main dumps chip/flash/PSRAM identity + heartbeats, and exercises the board.h display seam
# (a stub on the P4 board in GP-2). board.cpp is selected by BOARD_DIR and compiled into main, same
# pattern as the other apps.
idf_component_register(
    SRCS "main.cpp" "${BOARD_DIR}/board.cpp"
    INCLUDE_DIRS "." "${BOARD_DIR}"
    REQUIRES esp_psram esp_timer spi_flash esp_lcd esp_driver_gpio esp_mm esp_driver_i2c input
)
# main.cpp: esp_chip_info (esp_hw_support, transitive), esp_psram, esp_flash_get_size (spi_flash),
# esp_timer. GP-3 board.cpp adds the MIPI-DSI display path: esp_lcd (DSI bus/DBI/DPI), esp_driver_gpio
# (reset/backlight), esp_mm (esp_cache_msync for the PSRAM framebuffer). GP-5 adds GT911 touch:
# esp_driver_i2c (i2c_master). esp_ldo_regulator lives in esp_hw_support (always linked). All are IDF
# built-in components — none pull the repo components/ tree.

# Opt-in GT911 touch diagnostic: `make build ... DEFS='-D TOUCH_DEBUG=1'` logs the raw GT911 status
# register (rate-limited) so touch reporting can be debugged. Off by default.
if(DEFINED TOUCH_DEBUG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TOUCH_DEBUG=${TOUCH_DEBUG})
endif()
