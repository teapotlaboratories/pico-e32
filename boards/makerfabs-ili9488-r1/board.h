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
#include "sdcard_spi.h"   /* sdcard_spi_config_t — board_sd_config() fills this board's SD wiring */

/* Panel geometry, native portrait. */
#define BOARD_LCD_H_RES 320
#define BOARD_LCD_V_RES 480

/* This board has an onboard microSD slot (a private SPI2 bus). Boards without a slot simply never
 * define BOARD_HAS_SD, and the app's SD path compiles out. */
#define BOARD_HAS_SD 1

/* This board has an onboard FT6236 capacitive touch panel (I²C 0x38, SDA38/SCL39). Boards without touch
 * never define BOARD_HAS_TOUCH, so the touch input backend's board hooks are simply absent. */
#define BOARD_HAS_TOUCH 1

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

/* Fill *out with THIS board's microSD wiring (SPI host, pins, owns_bus) and return true; false if
 * the board has no usable card slot this boot. Writes ONLY the hardware fields — mount policy
 * (mount point, FAT options) stays the caller's. Symmetric with board_lcd_init: the board owns its
 * SD wiring the same way it owns its display. The app guards its *call* with BOARD_HAS_SD (which this
 * board defines); a board without an SD slot omits both the macro and this declaration. */
bool board_sd_config(sdcard_spi_config_t *out);

/* Bring up the FT6236 capacitive touch controller (I²C). ESP_OK on success; on failure it logs and
 * returns an error (touch reads then report no points). Call once. Symmetric with board_lcd_init — the
 * board owns the touch hardware AND its orientation. */
esp_err_t board_touch_init(void);

/* Poll active touch points (up to `max`, at most 2) into xs/ys in DISPLAY coordinates
 * (0..BOARD_LCD_H_RES-1, 0..BOARD_LCD_V_RES-1) — this panel's orientation (ROTATE_180) already applied, so
 * a point lands where it is drawn. Returns the point count (0..2). One I²C transaction; non-blocking-ish. */
int board_touch_read(int *xs, int *ys, int max);

/* Draw the on-screen touch control deck (d-pad + O/X + menu) once into the panel's bottom band, below the
 * game. Static — drawFrame writes only the top 256 px, so the deck persists without a per-frame redraw.
 * Layout matches docs/runtime/pico-e32-fake08-touch-ui.html. Only meaningful with BOARD_HAS_TOUCH. */
void board_draw_touch_deck(void);

/* Dev HUD: paint the current loop FPS in the right letterbox (x >= 288), which the centred 256px game
 * blit never overwrites — so it persists without a per-frame redraw and never covers gameplay. Called
 * only by SHOW_FPS builds. */
void board_lcd_draw_fps(int fps);

#ifdef __cplusplus
}
#endif
