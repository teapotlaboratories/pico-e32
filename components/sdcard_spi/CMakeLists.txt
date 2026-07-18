# Reusable, board-agnostic microSD-over-SPI mount. Knows SPI + FATFS, never a board.
# Public deps — types that appear in the public header sdcard_spi.h:
#   esp_driver_spi  → spi_host_device_t (driver/spi_common.h)
#   driver          → gpio_num_t        (driver/gpio.h)
# The FATFS / sdmmc / sdspi machinery is an implementation detail → PRIV_REQUIRES, so it does not
# leak onto dependents. Auto-discovered via each app's EXTRA_COMPONENT_DIRS (the repo-root components/).
idf_component_register(
    SRCS "sdcard_spi.c"
    INCLUDE_DIRS "."
    REQUIRES      driver esp_driver_spi
    PRIV_REQUIRES fatfs sdmmc esp_driver_sdspi
)
