/* fps_hud.cpp — portable on-screen FPS HUD for the fake-08 host, shared by ALL boards.
 *
 * Renders "<n> FPS" through the board-agnostic seam (board_lcd_blit + board_lcd_rgb565, declared in
 * fake08_board.h) and a bundled 3x5 font, so ONE implementation serves every board.h — the S3 (ILI9488 /
 * LovyanGFX) and the P4 (ST7701S / MIPI-DSI) alike, with no per-board text code. board.cpp no longer
 * implements board_lcd_draw_fps.
 *
 * Lives in the fake-08 component (parent-repo wrapper, NOT the vendored submodule) alongside ESP32Host,
 * which calls it — so the symbols resolve within one archive on every board. It depends only on the
 * board_lcd_* seam, never on a specific board.h, so it carries no geometry: the HUD sits at a fixed
 * TOP-LEFT margin, in the horizontal letterbox left when the integer-scaled game is narrower than the
 * panel (the game is flush to the top and centred horizontally on both boards). Only *called* in SHOW_FPS
 * builds.
 */
#include "fake08_board.h"    /* board_lcd_blit(), board_lcd_rgb565() — the board-agnostic seam */
#include "pico8_font.h"      /* {char c; const char *rows[5];} PICO8_FONT_GLYPHS[] — 3x5, '#' = pixel on */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#define GLYPH_W   3
#define GLYPH_H   5
#define HUD_SCALE 4                        /* on-screen pixels per font pixel */
#define ADV       (GLYPH_W + 1)            /* glyph + 1px gap, in font px */
#define CELL_W    (ADV * HUD_SCALE)        /* per-char advance on screen */
#define MAXCH     7                        /* fixed-width box sized for "999 FPS" so shorter strings clear */
#define BOX_W     (MAXCH * CELL_W)
#define BOX_H     (GLYPH_H * HUD_SCALE)
#define HUD_X     8                        /* top-left margin (in the top letterbox) */
#define HUD_Y     8

/* HUD ownership flag (was in the S3 board.cpp; shared here so every board's SHOW_FPS build links it).
 * ESP32Host's generic meter times the render loop; a play-test loop that knows the true game-frame count
 * sets this to take over the HUD and make the generic meter stand down. Referenced by ESP32Host + main.cpp. */
extern "C" { volatile int g_hud_owned_by_app = 0; }

static const char *const *glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);   /* PICO-8 renders lowercase as caps */
    for (size_t i = 0; i < sizeof(PICO8_FONT_GLYPHS) / sizeof(PICO8_FONT_GLYPHS[0]); i++)
        if (PICO8_FONT_GLYPHS[i].c == c) return PICO8_FONT_GLYPHS[i].rows;
    return nullptr;
}

extern "C" void board_lcd_draw_fps(int fps) {
    if (fps < 0) fps = 0;
    if (fps > 999) fps = 999;
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%d FPS", fps);
    if (n > MAXCH) n = MAXCH;

    uint16_t fg = fps >= 28 ? board_lcd_rgb565(0x7f, 0xe0, 0x9a)   /* at the 30 fps target */
                : fps >= 18 ? board_lcd_rgb565(0xf2, 0xc9, 0x60)   /* dipping */
                            : board_lcd_rgb565(0xe8, 0x6f, 0x6f);  /* struggling */
    uint16_t bg = board_lcd_rgb565(0x0d, 0x11, 0x17);

    static uint16_t px[BOX_W * BOX_H];             /* the HUD cell (fixed size; ~4.4 KB, .bss) */
    for (int i = 0; i < BOX_W * BOX_H; i++) px[i] = bg;

    int start = (MAXCH - n) / 2;                   /* centre the text within the fixed box */
    for (int ci = 0; ci < n; ci++) {
        const char *const *rows = glyph(buf[ci]);
        if (!rows) continue;
        int ox = (start + ci) * CELL_W;
        for (int ry = 0; ry < GLYPH_H; ry++)
            for (int rx = 0; rx < GLYPH_W; rx++)
                if (rows[ry][rx] == '#')
                    for (int sy = 0; sy < HUD_SCALE; sy++)
                        for (int sx = 0; sx < HUD_SCALE; sx++)
                            px[(ry * HUD_SCALE + sy) * BOX_W + (ox + rx * HUD_SCALE + sx)] = fg;
    }
    board_lcd_blit(HUD_X, HUD_Y, BOX_W, BOX_H, px);
}
