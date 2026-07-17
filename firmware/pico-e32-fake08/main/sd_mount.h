/* sd_mount.h — mount a microSD over SPI for the fake-08 cart loader (pico-e32). */
#pragma once

#include "esp_err.h"

#define SD_MOUNT_POINT "/sdcard"

/* Mount a microSD over SPI (SPI2 on this board: CS=GPIO1, MOSI=GPIO2, MISO=GPIO41, CLK=GPIO42) at
 * SD_MOUNT_POINT. Returns ESP_OK on success. On ANY failure it logs a warning, tears the bus down
 * cleanly, and returns the error — it never aborts, so the caller can fall back to a flash-embedded
 * cart. Safe to call once at boot. */
esp_err_t sd_mount(void);

/* Unmount + free the bus. Safe no-op if not mounted. */
void sd_unmount(void);
