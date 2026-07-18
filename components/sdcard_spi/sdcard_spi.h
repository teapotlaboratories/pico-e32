/* sdcard_spi.h — reusable, board-agnostic microSD-over-SPI mount for ESP-IDF.
 *
 * Knows only SPI + FATFS, never a board. Each board supplies its own wiring (SPI host, pins, and
 * whether it owns the SPI bus) through an sdcard_spi_config_t; the app supplies the mount policy
 * (where to mount, whether to format, fallback). Extracted from the pico-e32-fake08 app's original
 * sd_mount.cpp so any board/app can reuse it — see boards/<board>/board.cpp:board_sd_config() for
 * the per-board wiring.
 *
 * Graceful by contract: any failure logs a warning, unwinds only what the mount itself created, and
 * returns the error — it never aborts, so the caller can fall back (e.g. to a flash-embedded cart).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "driver/spi_common.h"   /* spi_host_device_t — a config field type */
#include "driver/gpio.h"         /* gpio_num_t — the pin field type */

#ifdef __cplusplus
extern "C" {
#endif

#define SDCARD_SPI_DEFAULT_MOUNT_POINT "/sdcard"

/* SD-over-SPI wiring + mount policy, split by owner:
 *   - a board fills the HARDWARE fields (host/pins/owns_bus) in board_sd_config();
 *   - the app owns the POLICY fields (mount_point, format, max_files, ...).
 * Get sane policy defaults from sdcard_spi_config_default(), then let the board overwrite the
 * hardware fields on top. */
typedef struct {
    /* --- hardware (board-owned) --- */
    spi_host_device_t host;        /* SPI host the card lives on (e.g. SPI2_HOST). */
    gpio_num_t pin_cs;             /* chip-select — ALWAYS used. */
    gpio_num_t pin_mosi;           /* used ONLY when owns_bus (ignored on a shared bus). */
    gpio_num_t pin_miso;           /* used ONLY when owns_bus. */
    gpio_num_t pin_sclk;           /* used ONLY when owns_bus. */
    bool owns_bus;                 /* true : this component runs spi_bus_initialize at mount and
                                    *        spi_bus_free at unmount (a PRIVATE bus).
                                    * false: the bus is already up (e.g. the display driver owns it) —
                                    *        attach the SD device only, and NEVER free the bus. */

    /* --- speed / bus sizing --- */
    int max_freq_khz;              /* per-device SPI clock (e.g. 20000 = SDMMC_FREQ_DEFAULT). */
    int max_transfer_sz;           /* bus transfer size; owns_bus only (e.g. 4000). */

    /* --- mount policy (app-owned) --- */
    bool format_if_mount_failed;   /* NEVER true for a user's card unless you truly mean it. */
    int max_files;                 /* max simultaneously open files. */
    size_t allocation_unit_size;   /* 0 == use the card's sector size. */
    const char *mount_point;       /* VFS base path, e.g. "/sdcard". */

    /* --- optional --- */
    gpio_num_t pin_cd;             /* card-detect;   GPIO_NUM_NC if unused. */
    gpio_num_t pin_wp;             /* write-protect; GPIO_NUM_NC if unused. */
} sdcard_spi_config_t;

/* Fill *out with safe defaults: NO wiring yet (pins = GPIO_NUM_NC, so an under-filled config fails
 * loud rather than silently driving GPIO0), owns_bus=true, no-format, 20 MHz, "/sdcard". Call this,
 * set your mount policy, then let board_sd_config() overwrite the hardware fields. */
void sdcard_spi_config_default(sdcard_spi_config_t *out);

/* Mount the card at cfg->mount_point. ESP_OK on success. On ANY failure it logs, unwinds only what it
 * created (frees the bus iff it initialized it), and returns the error — never aborts. Idempotent: a
 * second call while already mounted returns ESP_OK. Single-instance (one card at a time). */
esp_err_t sdcard_spi_mount(const sdcard_spi_config_t *cfg);

/* Unmount and — iff this component brought the bus up (owns_bus) — free it. A shared bus is left
 * running. Safe no-op if nothing is mounted. */
void sdcard_spi_unmount(void);

#ifdef __cplusplus
}
#endif
