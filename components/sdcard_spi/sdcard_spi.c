/* sdcard_spi.c — board-agnostic microSD-over-SPI mount. See sdcard_spi.h.
 *
 * Generalized from the pico-e32-fake08 app's original sd_mount.cpp: every fixed value is now a
 * config field, and the "who owns the SPI bus" assumption is explicit (owns_bus). Two teardown bugs
 * from the original are fixed here:
 *   1. the freed host is the one actually used (s_host), not a hardcoded SPI2_HOST literal;
 *   2. the bus is freed ONLY when this component initialized it (s_bus_owned) — a shared bus, or a
 *      mount that fails before the bus comes up, frees nothing.
 */
#include "sdcard_spi.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"

static const char *TAG = "sdcard_spi";

/* Single-instance state: one card at a time (every board here has a single slot). */
static sdmmc_card_t     *s_card = NULL;
static spi_host_device_t s_host;               /* the host actually used — freed at unmount iff owned */
static bool              s_bus_owned = false;  /* did WE call spi_bus_initialize? */
static char              s_mount_point[32];

void sdcard_spi_config_default(sdcard_spi_config_t *out) {
    sdcard_spi_config_t c = {
        .host     = SPI2_HOST,
        .pin_cs   = GPIO_NUM_NC, .pin_mosi = GPIO_NUM_NC,
        .pin_miso = GPIO_NUM_NC, .pin_sclk = GPIO_NUM_NC,
        .owns_bus = true,
        .max_freq_khz    = 20000,             /* SDMMC_FREQ_DEFAULT */
        .max_transfer_sz = 4000,
        .format_if_mount_failed = false,      /* NEVER auto-wipe a card */
        .max_files            = 5,
        .allocation_unit_size = 0,
        .mount_point = SDCARD_SPI_DEFAULT_MOUNT_POINT,
        .pin_cd = GPIO_NUM_NC, .pin_wp = GPIO_NUM_NC,
    };
    *out = c;
}

esp_err_t sdcard_spi_mount(const sdcard_spi_config_t *cfg) {
    if (s_card) return ESP_OK;                /* already mounted */

    s_host      = cfg->host;
    s_bus_owned = false;                       /* set true only after we bring the bus up */

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot         = cfg->host;
    host.max_freq_khz = cfg->max_freq_khz;

    if (cfg->owns_bus) {
        spi_bus_config_t bus = {
            .mosi_io_num     = cfg->pin_mosi,
            .miso_io_num     = cfg->pin_miso,
            .sclk_io_num     = cfg->pin_sclk,
            .quadwp_io_num   = -1,
            .quadhd_io_num   = -1,
            .max_transfer_sz = cfg->max_transfer_sz,
        };
        esp_err_t r = spi_bus_initialize(cfg->host, &bus, SPI_DMA_CH_AUTO);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "spi_bus_initialize failed: %s - continuing without SD", esp_err_to_name(r));
            return r;                          /* nothing created yet to unwind */
        }
        s_bus_owned = true;
    }
    /* Shared bus (owns_bus == false): the display driver already ran spi_bus_initialize; calling it
     * again would return ESP_ERR_INVALID_STATE. esp_vfs_fat_sdspi_mount() below only attaches a
     * device on cfg->pin_cs — it never touches the bus init — so it is safe on the live shared bus. */

    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = cfg->pin_cs;
    dev.gpio_cd = cfg->pin_cd;
    dev.gpio_wp = cfg->pin_wp;
    dev.host_id = cfg->host;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = cfg->format_if_mount_failed,
        .max_files              = cfg->max_files,
        .allocation_unit_size   = cfg->allocation_unit_size,
    };

    esp_err_t r = esp_vfs_fat_sdspi_mount(cfg->mount_point, &host, &dev, &mcfg, &s_card);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s - continuing without SD (check card + line pull-ups)",
                 esp_err_to_name(r));
        if (s_bus_owned) spi_bus_free(cfg->host);   /* free ONLY what we created */
        s_bus_owned = false;
        s_card      = NULL;
        return r;
    }

    snprintf(s_mount_point, sizeof(s_mount_point), "%s", cfg->mount_point);
    ESP_LOGI(TAG, "SD mounted at %s", cfg->mount_point);
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

void sdcard_spi_unmount(void) {
    if (!s_card) return;
    esp_vfs_fat_sdcard_unmount(s_mount_point, s_card);
    if (s_bus_owned) spi_bus_free(s_host);          /* the crux: a shared bus is left running */
    s_card      = NULL;
    s_bus_owned = false;
}
