/* Board display API — Makerfabs ESP32-S3 Parallel TFT 3.5" (ILI9488), first revision.
 *
 * The board-agnostic surface an app draws through. Names carry NO vendor/revision/driver: an app
 * says "the board's LCD", and which board.{h,cpp} it gets is chosen by BOARD at build time
 * (-D BOARD_DIR, see the repo Makefile). Swapping boards is a boards/<board>/board.{h,cpp} swap,
 * not an app edit. The implementation (pins, orientation, byte order, the LovyanGFX bring-up) lives
 * in board.cpp; this header is only the contract.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Panel geometry, native portrait. */
#define BOARD_LCD_H_RES 320
#define BOARD_LCD_V_RES 480

/* Pixel-clock (WR strobe) of the parallel bus, in Hz. Exposed because a benchmark needs it to
 * derive the bus ceiling honestly rather than pasting a constant (a stale one is how a 20 MHz
 * ceiling once sat next to a 40 MHz measurement). */
#define BOARD_LCD_PCLK_HZ (40 * 1000 * 1000)

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the bus + panel + backlight. ESP_OK on success. */
esp_err_t board_lcd_init(void);

/* Blit an RGB565 rectangle (panel pixels). Blocks until the transfer has fully drained, so `src`
 * may be reused immediately on return. Feed it colours built with board_lcd_rgb565 so the byte
 * order matches this board's bus. */
void board_lcd_blit(int x, int y, int w, int h, const uint16_t *src);

/* Fill the whole panel with one RGB565 colour (e.g. board_lcd_rgb565(0,0,0)). */
void board_lcd_fill(uint16_t color);

/* RGB888 -> RGB565 in the byte order this board's bus needs. Build palettes with this, never by
 * open-coding the shift, so the swap policy has one definition. */
uint16_t board_lcd_rgb565(uint8_t r, uint8_t g, uint8_t b);

/* Cycle the panel red/green/blue via the library's own API, bypassing the framebuffer/blit path.
 * Diagnostic only; blocks ~9s. */
void board_lcd_selftest(void);

#ifdef __cplusplus
}
#endif
