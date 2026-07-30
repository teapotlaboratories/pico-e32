/* Board display API — Guition JC4880P443C-I-W, 4.3" 480x800 IPS (ST7701S over MIPI-DSI), ESP32-P4.
 *
 * Same board-agnostic surface the ESP32-S3 board exposes (see boards/makerfabs-ili9488-r1/board.h):
 * an app says "the board's LCD" and BOARD picks which board.{h,cpp} it links at build time
 * (-D BOARD_DIR, see the repo Makefile). Swapping this P4 board for the S3 board is a
 * boards/<board>/ swap, not an app edit; the header is only the contract, the implementation
 * (MIPI-DSI bring-up, ST7701S init, PSRAM framebuffer) lives in board.cpp.
 *
 * GP-3: board.cpp implements the display over MIPI-DSI (ST7701S), hardware-verified. GP-5: GT911
 * capacitive touch (I2C). The pins/timing/init are sourced from ESPHome's board model + confirmed on
 * hardware — see board.cpp's header. This board has no microSD slot, so BOARD_HAS_SD is not defined and
 * an app's SD path compiles out. ES8311 audio comes next. See docs/hardware/pico-e32-guition-jc4880p443c-p4.md.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Panel geometry, native portrait (480 wide x 800 tall). */
#define BOARD_LCD_H_RES 480
#define BOARD_LCD_V_RES 800

/* This board has an onboard GT911 capacitive touch panel (I2C 0x5D, SDA7/SCL8). */
#define BOARD_HAS_TOUCH 1

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the DPHY LDO + DSI bus + ST7701S panel + backlight. ESP_OK on success; on failure it logs
 * the failing stage and returns the error. */
esp_err_t board_lcd_init(void);

/* Blit an RGB565 rectangle (panel pixels) via the DPI panel. Feed it colours built with
 * board_lcd_rgb565 so the byte order matches this board's framebuffer. */
void board_lcd_blit(int x, int y, int w, int h, const uint16_t *src);

/* Fill the whole panel with one RGB565 colour. */
void board_lcd_fill(uint16_t color);

/* RGB888 -> RGB565 in the byte order this board's framebuffer needs. Build palettes with this, never by
 * open-coding the shift, so the swap policy has one definition. */
uint16_t board_lcd_rgb565(uint8_t r, uint8_t g, uint8_t b);

/* Panel dimensions in pixels (BOARD_LCD_H_RES / V_RES). Runtime getters so board-agnostic code (e.g. the
 * portable touch deck) can compute layout without including this board.h. */
int board_lcd_width(void);
int board_lcd_height(void);

/* Dev: the live DPI scan-out framebuffer (RGB565, w*h) — the exact pixels on the panel, for a camera-free
 * "screenshot over serial". Returns NULL before board_lcd_init(). w/h out (may be NULL). */
const uint16_t *board_lcd_framebuffer(int *w, int *h);

/* Bring up the GT911 capacitive touch controller (I2C). ESP_OK on success; on failure it logs and
 * returns an error (touch reads then report no points). Call once. Symmetric with board_lcd_init — the
 * board owns the touch hardware. Only meaningful with BOARD_HAS_TOUCH. */
esp_err_t board_touch_init(void);

/* Poll active touch points (up to `max`, at most 5) into xs/ys in DISPLAY coordinates
 * (0..BOARD_LCD_H_RES-1, 0..BOARD_LCD_V_RES-1). Returns the point count (0..5). One I2C transaction path.
 * NOTE (GP-5): touch<->display axis alignment is not yet HITL-confirmed — coords are passed through raw. */
int board_touch_read(int *xs, int *ys, int max);

#ifdef __cplusplus
}
#endif
