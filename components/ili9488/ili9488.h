/* Minimal ILI9488 driver over the ESP32-S3 esp_lcd i80 (16-bit Intel-8080) bus.
 *
 * Reusable display component: bus + panel-IO bring-up, the ILI9488 init sequence,
 * and a DMA blit that blocks on completion. Board-specific pins/orientation come in
 * via ili9488_config_t, so the same driver serves any 16-bit-parallel ILI9488 board.
 *
 * Verified at the bus level on the Makerfabs ILI9488 (Gate #1: 288 fps @ 256x256,
 * ~94% of the 20 MHz x 16-bit bus). Panel colour/orientation correctness is a
 * separate visual check (needs the bench camera). */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      pin_wr;              /* WR strobe */
    int      pin_dc;             /* data/command (RS) */
    int      pin_cs;             /* chip select (-1 if the board ties it low) */
    int      pin_bl;             /* backlight (PWM via LEDC); -1 to skip */
    int      data_pins[16];      /* 16-bit parallel data bus */
    int      pclk_hz;            /* pixel clock (ILI9488 tolerates up to ~20 MHz) */
    int      h_res;              /* panel width  (native, pre-rotation of MADCTL) */
    int      v_res;              /* panel height */
    uint8_t  madctl;             /* 0x36 value: orientation + RGB/BGR order */
    bool     swap_color_bytes;   /* swap the two RGB565 bytes on the 16-bit bus */
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

/* RGB888 -> RGB565, honouring the configured swap_color_bytes. */
uint16_t ili9488_rgb565(uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif
