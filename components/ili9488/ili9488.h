/* ILI9488 driver over a 16-bit Intel-8080 parallel bus, implemented with LovyanGFX.
 *
 * Reusable display component: bus + panel bring-up and a blit that blocks until the
 * transfer has drained. The panel bring-up is delegated to LovyanGFX's Panel_ILI9488
 * (see ili9488.cpp for why LovyanGFX and not esp_lcd); board-specific facts -- pins,
 * geometry, panel mounting -- come in via ili9488_config_t.
 *
 * A previous hand-rolled init sequence passed every non-visual check -- 288 fps, DMA
 * completing, framebuffer checksums byte-identical to the host build -- while putting
 * NOTHING on the glass, because it was driving the wrong pins. Only a camera pointed at
 * the panel caught it. See docs/worklog/2026-07-16-panel-rev1-pinmap.md. */
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
    int      pin_rd;             /* RD strobe -- MUST be driven high on an 8080 bus.
                                  * -1 if the board ties it high. */
    int      data_pins[16];      /* 16-bit parallel data bus */
    int      pclk_hz;            /* pixel clock (rev-1 source of record: 40 MHz) */
    int      h_res;              /* panel width */
    int      v_res;              /* panel height */
    bool     mirror_y;           /* BOARD FACT: the glass is mounted vertically mirrored
                                  * relative to the controller's native scan direction, so
                                  * row 0 of GRAM lands at the BOTTOM of the panel. True for
                                  * the Makerfabs rev-1 3.5" board -- measured, see
                                  * docs/worklog/2026-07-16-yflip-and-gate1-fps.md. The driver
                                  * corrects it in the panel's MADCTL, at zero per-frame cost.
                                  * Replaces a dead `madctl` byte that no driver ever read. */
    bool     swap_color_bytes;   /* Which side does the RGB565 byte swap, NOT whether one
                                  * happens. true  -> the app pre-swaps (via ili9488_rgb565)
                                  * and the driver hands the panel the bytes as-is;
                                  * false -> the app keeps native order and the driver swaps
                                  * per pixel, which is markedly slower. The pixels on the
                                  * glass are the same either way -- this is a performance
                                  * knob wearing a correctness name. Keep app and driver
                                  * agreed: ili9488_rgb565() honours this field, so build
                                  * palettes with it rather than open-coding the shift. */
    size_t   max_transfer_bytes; /* largest single blit; LovyanGFX manages its own DMA
                                  * descriptors, so this is advisory only today. */
} ili9488_config_t;

/* Bring up the parallel bus + panel, run the ILI9488 init sequence, enable backlight.
 * ESP_OK on success. A copy of cfg is retained. */
esp_err_t ili9488_init(const ili9488_config_t *cfg);

/* Blit an RGB565 rectangle. Blocks until the transfer has fully drained, so `src` may be
 * reused immediately after return. Coordinates are in panel pixels.
 *
 * The drain is load-bearing and subtle: it happens because each blit is its own LovyanGFX
 * transaction, so the unnested endWrite() calls _bus->wait(). Wrapping several blits in an
 * outer startWrite()/endWrite() -- the usual LovyanGFX efficiency idiom -- would move the
 * drain to the outer endWrite, silently turning any surrounding fps loop into a measurement
 * of the QUEUEING rate rather than the transfer rate. This project has already published one
 * fps number that measured nothing; don't reintroduce the shape of that bug. */
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
