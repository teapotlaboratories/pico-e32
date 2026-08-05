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
 * hardware — see board.cpp's header. This board has a TF/microSD slot wired to the ESP32-P4 SDMMC
 * peripheral (slot 0, 4-bit) — BOARD_HAS_SDMMC below; SD-over-SPI (BOARD_HAS_SD) is a different seam the
 * S3 uses. ES8311 audio too. See docs/hardware/pico-e32-guition-jc4880p443c-p4.md.
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

/* On-screen touch control deck — the BOARD owns both the layout (where the d-pad / O / X / MENU sit on
 * this panel) and the rendering. The P4 rasterises the deck into its DPI framebuffer via board_lcd_blit
 * (parity-agnostic — no i80 DMA quirk). The shared input layer (components/input/input_touch.c) just
 * calls these; screen geometry never leaks into it. */
void    board_draw_touch_deck(void);          /* paint the static deck once (after board_touch_init) */
uint8_t board_touch_hittest(int x, int y);    /* map a touch (display coords) to an INPUT_* bit; 0 = none */

/* This board HAS audio output: an ES8311 codec (I2C 0x18 on the shared touch bus) + I2S. board_audio_init()
 * brings up the codec + I2S TX; board_audio_write() plays `frames` stereo S16 frames, blocking until queued. */
#define BOARD_HAS_AUDIO 1
esp_err_t board_audio_init(void);
void      board_audio_write(const int16_t *stereo, size_t frames);

/* This board HAS a TF/microSD slot on the ESP32-P4 SDMMC peripheral (slot 0, 4-bit, IO_MUX). Unlike the S3's
 * SD-over-SPI (BOARD_HAS_SD, sdcard_spi), SDMMC is a native host with its own mount path, so the board owns
 * the WHOLE mount (esp_vfs_fat_sdmmc_mount) behind this one seam — the app just asks for a mount point and
 * gets a FAT VFS. ESP_OK on success; any error means "no card / mount failed" and the app falls back to the
 * flash cart. Idempotent-ish: a second call while mounted returns ESP_ERR_INVALID_STATE. */
#define BOARD_HAS_SDMMC 1
esp_err_t board_sd_mount(const char *mount_point);

/* Carousel-launcher layout for THIS panel (display pixels). The launcher reads these instead of hardcoding
 * positions, so the same UI code lays out correctly on any board. The game column [game_x, game_x+game_w)
 * must match where the fake-08 host actually renders the game (128 * integer upscale, horizontally centred),
 * so launcher content stays inside the game's on-screen footprint. */
typedef struct {
    int game_x, game_w;    /* game column (content is confined here) */
    int thumb_w, thumb_h;  /* centre cover thumbnail (portrait, ~160:205) */
    int thumb_y;           /* thumbnail top */
    int side_w;            /* side peek width */
    int crumb_y;           /* breadcrumb / screen-header top */
    /* Main-menu + settings/about layout (all kept ABOVE the touch deck, whose position is board-specific). */
    int title_y, title_scale;  /* main-menu big "PICO-E32" title: top + glyph scale */
    int body_y, body_dy;       /* menu items / settings rows: first-row top + row spacing */
    int body_scale;            /* menu-item (Games/Settings/About) glyph scale — sized to the panel */
    int info_scale;            /* Settings/About body-text glyph scale — sized to the panel */
} board_carousel_layout_t;
void board_carousel_layout(board_carousel_layout_t *out);

#ifdef __cplusplus
}
#endif
