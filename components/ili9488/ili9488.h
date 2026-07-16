/* ILI9488 driver over the ESP32-S3 esp_lcd i80 (16-bit Intel-8080) bus.
 *
 * Reusable display component: bus + panel-IO bring-up and a DMA blit that blocks on
 * completion. The panel bring-up itself is delegated to the proven
 * `atanisoft/esp_lcd_ili9488` component; board-specific pins come in via
 * ili9488_config_t.
 *
 * A previous hand-rolled init sequence passed every non-visual check -- 288 fps, DMA
 * completing, framebuffer checksums byte-identical to the host build -- while putting
 * NOTHING on the glass. Only a camera pointed at the panel caught it. Hence the
 * component: see docs/worklog/2026-07-15-bench-camera.md. */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      pin_wr;             /* WR strobe */
    int      pin_dc;             /* data/command (RS) */
    int      pin_cs;             /* chip select (-1 if the board ties it low) */
    int      pin_bl;             /* backlight (PWM via LEDC); -1 to skip */
    int      pin_rd;             /* RD strobe -- MUST be driven high on an 8080 bus;
                                  * esp_lcd never touches it. -1 if the board ties it high. */
    int      data_pins[16];      /* 16-bit parallel data bus */
    int      pclk_hz;            /* pixel clock (Makerfabs' own example uses 10 MHz) */
    int      h_res;              /* panel width */
    int      v_res;              /* panel height */
    uint8_t  madctl;             /* UNUSED: the panel component owns MADCTL (it sets BGR +
                                  * orientation). Kept so board configs still compile; use
                                  * esp_lcd_panel_mirror/swap_xy if orientation needs work. */
    bool     swap_color_bytes;   /* swap the two RGB565 bytes (applied in ili9488_rgb565) */
    size_t   max_transfer_bytes; /* largest single blit, sizes the i80 DMA path */
} ili9488_config_t;

/* Bring up the i80 bus + panel IO, run the ILI9488 init sequence, enable backlight.
 * Returns the first esp_lcd error (ESP_OK on success). A copy of cfg is retained. */
esp_err_t ili9488_init(const ili9488_config_t *cfg);

/* Blit an RGB565 rectangle. Blocks until the DMA completes, so `src` may be reused
 * immediately after return. Coordinates are in panel pixels. */
void ili9488_blit(int x, int y, int w, int h, const uint16_t *src);

/* Fill the whole panel with one RGB565 colour (e.g. ili9488_rgb565(0,0,0)). */
void ili9488_fill(uint16_t color);

/* Self-test: cycle the panel red/green/blue using LovyanGFX's own API, bypassing this
 * project's framebuffer/palette/blit path entirely. Blocks ~9s. Diagnostic only. */
void ili9488_selftest(void);

/* RGB888 -> RGB565, honouring the configured swap_color_bytes. */
uint16_t ili9488_rgb565(uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif
