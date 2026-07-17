/* sd_mount.cpp — mount a microSD over SPI for the pico-e32-fake08 cart loader.
 *
 * Pins on this board (boards/makerfabs-ili9488-r1) are disjoint from the i80 LCD (WR35/DC36/CS37 + data
 * {47,21,14,13,12,11,10,9,3,8,16,15,7,6,5,4}), so the SD gets a private SPI2 bus — no contention. The
 * "microSD bus shared with the LCD" note in the board reference is a carry-over from the 4" board (DP-7).
 *
 * Graceful by contract: any failure (no card, no pull-ups, bad FAT) logs a warning, frees the bus, and
 * returns the error. app_main then falls back to the flash-embedded cart. Never aborts boot. */
#include "sd_mount.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"

#define SD_PIN_CS   GPIO_NUM_1
#define SD_PIN_MOSI GPIO_NUM_2
#define SD_PIN_MISO GPIO_NUM_41
#define SD_PIN_CLK  GPIO_NUM_42

static const char *TAG = "sd";
static sdmmc_card_t *s_card = nullptr;

esp_err_t sd_mount(void) {
    if (s_card) return ESP_OK; /* already mounted */

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();   /* host.slot = SPI2_HOST on the ESP32-S3 */
    /* host.max_freq_khz = 10000; */            /* drop from the 20 MHz default if the wiring is long/flaky */
    spi_host_device_t slot = (spi_host_device_t)host.slot;

    spi_bus_config_t bus = {};
    bus.mosi_io_num     = SD_PIN_MOSI;
    bus.miso_io_num     = SD_PIN_MISO;
    bus.sclk_io_num     = SD_PIN_CLK;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = 4000;

    esp_err_t ret = spi_bus_initialize(slot, &bus, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "spi_bus_initialize failed: %s - continuing without SD", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = SD_PIN_CS;
    dev.host_id = slot;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {};
    mcfg.format_if_mount_failed = false; /* NEVER auto-wipe a card */
    mcfg.max_files              = 5;
    mcfg.allocation_unit_size   = 0;

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &dev, &mcfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s - continuing without SD (check card + line pull-ups)",
                 esp_err_to_name(ret));
        spi_bus_free(slot);
        s_card = nullptr;
        return ret;
    }
    ESP_LOGI(TAG, "SD mounted at %s", SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

void sd_unmount(void) {
    if (!s_card) return;
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    spi_bus_free(SPI2_HOST);
    s_card = nullptr;
}
