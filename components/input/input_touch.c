/* input_touch.c — capacitive-touch input backend, PORTABLE across boards (IN-2).
 *
 * The board owns the touch controller + this panel's orientation via board_touch_init / board_touch_read
 * (points already in DISPLAY coordinates). This file owns the rest, and does it board-agnostically:
 *   - the control-deck LAYOUT, computed from the panel size (board_lcd_width/height) so one set of zones
 *     fits any panel. The proportions reproduce the approved 320x480 mockup EXACTLY at W=320,deckH=224
 *     (docs/runtime/pico-e32-fake08-touch-ui.html) and scale to e.g. the P4's 480x800.
 *   - the MAPPING: a touch point -> a zone -> a PICO-8 button bit, OR'd into input_poll().
 *   - the DECK RENDER: rasterised through the board_lcd_blit + board_lcd_rgb565 seam (no per-board drawing
 *     code — the S3's LovyanGFX version was removed), with O/X/MENU labels from assets/pico8_font.h.
 *
 * Screen is the top 256x256 (game flush to the top); the deck is the band below. See
 * docs/runtime/pico-e32-fake08-input.md (IN-2). */
#include "input.h"
#include "esp_log.h"
#include "pico8_font.h"     /* {char c; const char *rows[5];} PICO8_FONT_GLYPHS[] — 3x5, '#' = pixel on */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Board seam — resolved at the final app link (like board_lcd_*). A board with no touch omits
 * board_touch_* so an INPUT_BACKEND=touch build link-fails there: the intended signal. */
extern esp_err_t board_touch_init(void);
extern int       board_touch_read(int *xs, int *ys, int max);
extern void      board_lcd_blit(int x, int y, int w, int h, const uint16_t *src);
extern uint16_t  board_lcd_rgb565(uint8_t r, uint8_t g, uint8_t b);
extern int       board_lcd_width(void);
extern int       board_lcd_height(void);

static const char *TAG = "input.touch";
static bool s_ok;

/* Deck layout, derived from panel geometry (see file header). */
static struct {
    int dpad_cx, dpad_cy, reach, dead, barH, barW;
    int o_cx, o_cy, x_cx, x_cy, btn_r;
    int menu_cx, menu_cy, menu_w, menu_h;
    int lbl;   /* label glyph scale */
} L;

static void layout_init(void) {
    int W = board_lcd_width();
    int H = board_lcd_height();
    /* Game height = 128 * integer scale, and this MUST match ESP32Host's pico_scale(W) (same rule) so the
     * deck sits exactly below the game: S3 320 -> 2x -> 256; P4 480 -> 3x -> 384. */
    int scale = W / 128; if (scale < 2) scale = 2; if (scale > 3) scale = 3;
    int game_h = 128 * scale;
    int deckH = H - game_h; if (deckH < 0) deckH = 0;
    L.dpad_cx = W * 23 / 80;          L.dpad_cy = game_h + deckH * 120 / 224;
    L.reach   = 74 * W / 320;         L.dead    = 16 * W / 320;
    L.barH    = 50 * W / 320;         L.barW    = 50 * W / 320;
    L.o_cx    = W * 53 / 80;          L.o_cy    = game_h + deckH * 158 / 224;
    L.x_cx    = W * 68 / 80;          L.x_cy    = game_h + deckH * 96 / 224;
    L.btn_r   = 31 * W / 320;
    L.menu_cx = W / 2;                L.menu_cy = game_h + deckH * 27 / 224;
    L.menu_w  = 58 * W / 320;         L.menu_h  = 22 * W / 320;
    L.lbl     = 2 * W / 320; if (L.lbl < 1) L.lbl = 1;
}

static inline int sq(int v) { return v * v; }

/* One touch point (display coords) -> a single button bit (0 if it hits nothing). */
static uint8_t point_to_button(int x, int y) {
    int dx = x - L.dpad_cx, dy = y - L.dpad_cy;
    if (dx > -L.reach && dx < L.reach && dy > -L.reach && dy < L.reach) {
        if (sq(dx) + sq(dy) < sq(L.dead)) return 0;             /* dead hub */
        int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;     /* dominant axis picks the direction */
        if (ax > ay) return dx < 0 ? INPUT_LEFT : INPUT_RIGHT;
        return dy < 0 ? INPUT_UP : INPUT_DOWN;
    }
    int hit = L.btn_r + L.btn_r / 6;                            /* hit radius a touch larger than drawn */
    if (sq(x - L.o_cx) + sq(y - L.o_cy) < sq(hit)) return INPUT_O;
    if (sq(x - L.x_cx) + sq(y - L.x_cy) < sq(hit)) return INPUT_X;
    if (x >= L.menu_cx - L.menu_w / 2 && x <= L.menu_cx + L.menu_w / 2 &&
        y >= L.menu_cy - L.menu_h / 2 && y <= L.menu_cy + L.menu_h / 2) return INPUT_PAUSE;
    return 0;
}

/* ---- portable deck rendering (board_lcd_blit only) — the letterbox below the game is already black
 * (host oneTimeSetup fills it), so we draw just the controls; each shape is a small blitted buffer. ---- */

/* Linear colour interpolation -> board RGB565, for the gradients (matches the mockup's fills). */
static uint16_t lerp565(int r0, int g0, int b0, int r1, int g1, int b1, int num, int den) {
    if (den <= 0) den = 1;
    if (num < 0) num = 0;
    if (num > den) num = den;
    return board_lcd_rgb565((uint8_t)(r0 + (r1 - r0) * num / den),
                            (uint8_t)(g0 + (g1 - g0) * num / den),
                            (uint8_t)(b0 + (b1 - b0) * num / den));
}

/* Deck surface colour — a subtle dark blue-grey (the mockup's deck tone), NOT pure black. Control buffers
 * use it as their background so shapes sit on the surface; it must match ESP32Host's one-time panel fill. */
static uint16_t deck_bg(void) { return board_lcd_rgb565(0x0f, 0x14, 0x1d); }

/* Rounded-rect membership: is local pixel (rx,ry) inside a w×h rect with corner radius cr? */
static int in_rrect(int rx, int ry, int w, int h, int cr) {
    if (cr <= 0) return 1;
    int ccx = rx < cr ? cr : (rx >= w - cr ? w - 1 - cr : -1);
    int ccy = ry < cr ? cr : (ry >= h - cr ? h - 1 - cr : -1);
    if (ccx < 0 || ccy < 0) return 1;              /* not in a corner region -> inside */
    int dx = rx - ccx, dy = ry - ccy;
    return dx * dx + dy * dy <= cr * cr;
}

/* Small direction chevron into buffer b (W×H). dir 0/1/2/3 = up/down/left/right; apex at (ax,ay),
 * base 2*hw across, hh from apex to base — matches the mockup's small triangle glyphs. */
static void chevron(uint16_t *b, int W, int H, int dir, int ax, int ay, int hw, int hh, uint16_t col) {
    if (hh <= 0) return;
    for (int r = 0; r <= hh; r++) {
        int half = hw * r / hh;
        for (int e = -half; e <= half; e++) {
            int px, py;
            if (dir == 0)      { px = ax + e; py = ay + r; }   /* up: apex top, widen down */
            else if (dir == 1) { px = ax + e; py = ay - r; }   /* down: apex bottom, widen up */
            else if (dir == 2) { px = ax + r; py = ay + e; }   /* left: apex left, widen right */
            else               { px = ax - r; py = ay + e; }   /* right: apex right, widen left */
            if (px >= 0 && px < W && py >= 0 && py < H) b[py * W + px] = col;
        }
    }
}

/* Rounded-rect OUTLINE (border only; interior = bg) — the MENU pill is fill:none in the mockup. */
static void rrect_outline(int x, int y, int w, int h, int cr, int t, uint16_t col, uint16_t bg) {
    if (w <= 0 || h <= 0) return;
    uint16_t *b = (uint16_t *)malloc((size_t)w * h * 2); if (!b) return;
    for (int ry = 0; ry < h; ry++)
        for (int rx = 0; rx < w; rx++) {
            int inside = in_rrect(rx, ry, w, h, cr);
            int inner  = (rx >= t && ry >= t && rx < w - t && ry < h - t) &&
                         in_rrect(rx - t, ry - t, w - 2 * t, h - 2 * t, cr - t);
            b[ry * w + rx] = (inside && !inner) ? col : bg;
        }
    board_lcd_blit(x, y, w, h, b); free(b);
}

/* O/X button: a SPHERICAL radial shade (highlight offset up, #2f3743 -> #161b23) + a thin coloured ring,
 * with a clean vector glyph ('O' ring / 'X' cross) composited on top. Matches the mockup's "btn". */
static void disc_sphere(int cx, int cy, int r, uint16_t ring, uint16_t bg, char label, uint16_t lbl) {
    int s = 2 * r + 1, rr = r * r, hy = r * 35 / 100;   /* highlight sits above centre */
    uint16_t *b = (uint16_t *)malloc((size_t)s * s * 2); if (!b) return;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            uint16_t c = bg;
            if (d2 <= rr) {
                int hdy = dy + hy;                        /* distance from the highlight point (0,-hy) */
                int t = (dx * dx + hdy * hdy) * 95 / rr;  /* squared falloff reads as a soft sphere */
                c = lerp565(0x2f, 0x37, 0x43, 0x16, 0x1b, 0x23, t, 95);
                if (d2 >= (r - 2) * (r - 2)) c = ring;    /* thin coloured ring */
            }
            b[(dy + r) * s + (dx + r)] = c;
        }
    int th = r / 9; if (th < 1) th = 1;                   /* glyph stroke half-thickness */
    if (label == 'O') {
        int rg = r * 42 / 100;                            /* glyph ring radius */
        for (int dy = -rg - th; dy <= rg + th; dy++)
            for (int dx = -rg - th; dx <= rg + th; dx++) {
                int d2 = dx * dx + dy * dy;
                if (d2 <= (rg + th) * (rg + th) && d2 >= (rg - th) * (rg - th))
                    b[(dy + r) * s + (dx + r)] = lbl;
            }
    } else if (label == 'X') {
        int len = r * 40 / 100;
        for (int u = -len; u <= len; u++)
            for (int w2 = -th; w2 <= th; w2++) {
                int y1 = r + u + w2, x1 = r + u;          /* '\' diagonal */
                int y2 = r - u + w2, x2 = r + u;          /* '/' diagonal */
                if (x1 >= 0 && x1 < s && y1 >= 0 && y1 < s) b[y1 * s + x1] = lbl;
                if (x2 >= 0 && x2 < s && y2 >= 0 && y2 < s) b[y2 * s + x2] = lbl;
            }
    }
    board_lcd_blit(cx - r, cy - r, s, s, b); free(b);
}

/* D-pad: two vertically-graded bars forming a cross, four soft-blue direction chevrons, a dark hub —
 * matches the mockup's linearGradient "pad" + chevron paths. */
static void draw_dpad(void) {
    int cx = L.dpad_cx, cy = L.dpad_cy, reach = L.reach;
    int top = cy - reach, bot = cy + reach;             /* gradient spans the whole cross */
    uint16_t chev = board_lcd_rgb565(0x7f, 0xa8, 0xe6);
    uint16_t bg = deck_bg();
    int chh = L.barH / 5; if (chh < 3) chh = 3;         /* chevron height (apex -> base) */
    int chw = L.barH * 4 / 25; if (chw < 2) chw = 2;    /* chevron half-base width */
    int inset = L.barH * 8 / 25;                        /* chevron apex inset from each arm tip */

    /* horizontal bar (rounded) + left/right chevrons */
    { int x0 = cx - reach, y0 = cy - L.barH / 2, w = 2 * reach, h = L.barH;
      int cr = h * 2 / 5;                              /* rounded ends, matching the mockup's rx */
      uint16_t *b = (uint16_t *)malloc((size_t)w * h * 2); if (!b) return;
      for (int ry = 0; ry < h; ry++) {
          uint16_t col = lerp565(0x33, 0x3c, 0x4b, 0x1a, 0x20, 0x29, (y0 + ry) - top, bot - top);
          for (int rx = 0; rx < w; rx++) b[ry * w + rx] = in_rrect(rx, ry, w, h, cr) ? col : bg;
      }
      chevron(b, w, h, 2, inset, h / 2, chw, chh, chev);            /* left  (apex at left tip)  */
      chevron(b, w, h, 3, w - 1 - inset, h / 2, chw, chh, chev);    /* right (apex at right tip) */
      board_lcd_blit(x0, y0, w, h, b); free(b);
    }
    /* vertical bar (rounded) + up/down chevrons + hub (drawn 2nd so it caps the cross centre) */
    { int x0 = cx - L.barW / 2, y0 = cy - reach, w = L.barW, h = 2 * reach;
      int cr = w * 2 / 5;                             /* rounded ends */
      uint16_t *b = (uint16_t *)malloc((size_t)w * h * 2); if (!b) return;
      for (int ry = 0; ry < h; ry++) {
          uint16_t col = lerp565(0x33, 0x3c, 0x4b, 0x1a, 0x20, 0x29, (y0 + ry) - top, bot - top);
          for (int rx = 0; rx < w; rx++) b[ry * w + rx] = in_rrect(rx, ry, w, h, cr) ? col : bg;
      }
      chevron(b, w, h, 0, w / 2, inset, chw, chh, chev);            /* up   (apex at top tip)    */
      chevron(b, w, h, 1, w / 2, h - 1 - inset, chw, chh, chev);    /* down (apex at bottom tip) */
      uint16_t hubc = board_lcd_rgb565(0x14, 0x19, 0x22);          /* subtle dark hub (design r14 @ ~.55) */
      int hub = L.dead * 7 / 8;
      for (int dy = -hub; dy <= hub; dy++) for (int dx = -hub; dx <= hub; dx++)
          if (dx * dx + dy * dy <= hub * hub) {
              int px = w / 2 + dx, py = h / 2 + dy;
              if (px >= 0 && px < w && py >= 0 && py < h) b[py * w + px] = hubc;
          }
      board_lcd_blit(x0, y0, w, h, b); free(b);
    }
}
static const char *const *glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (size_t i = 0; i < sizeof(PICO8_FONT_GLYPHS) / sizeof(PICO8_FONT_GLYPHS[0]); i++)
        if (PICO8_FONT_GLYPHS[i].c == c) return PICO8_FONT_GLYPHS[i].rows;
    return NULL;
}
static void text(int cx, int cy, const char *s, uint16_t fg, uint16_t bg, int sc) {
    int n = (int)strlen(s), cw = 4 * sc, tw = n * cw, th = 5 * sc;
    if (tw <= 0) return;
    uint16_t *b = (uint16_t *)malloc((size_t)tw * th * 2); if (!b) return;
    for (int i = 0; i < tw * th; i++) b[i] = bg;
    for (int ci = 0; ci < n; ci++) {
        const char *const *rows = glyph(s[ci]); if (!rows) continue;
        int ox = ci * cw;
        for (int ry = 0; ry < 5; ry++)
            for (int rx = 0; rx < 3; rx++)
                if (rows[ry][rx] == '#')
                    for (int yy = 0; yy < sc; yy++)
                        for (int xx = 0; xx < sc; xx++)
                            b[(ry * sc + yy) * tw + (ox + rx * sc + xx)] = fg;
    }
    board_lcd_blit(cx - tw / 2, cy - th / 2, tw, th, b); free(b);
}

static void draw_deck(void) {
    uint16_t bg    = deck_bg();                            /* deck surface (not pure black) */
    uint16_t orng  = board_lcd_rgb565(0xe7, 0x9a, 0xa0);
    uint16_t xrng  = board_lcd_rgb565(0x5f, 0xc4, 0xbb);
    uint16_t menu  = board_lcd_rgb565(0x4a, 0x55, 0x66);
    uint16_t olbl  = board_lcd_rgb565(0xf3, 0xd6, 0xd8);
    uint16_t xlbl  = board_lcd_rgb565(0xcf, 0xee, 0xea);
    uint16_t mlbl  = board_lcd_rgb565(0x9a, 0xa6, 0xb6);   /* muted MENU label (design #9aa6b6) */
    /* d-pad: graded rounded cross + chevrons + hub */
    draw_dpad();
    /* O / X spherical buttons (thin coloured ring) + a clean vector glyph, on a gamepad diagonal */
    disc_sphere(L.o_cx, L.o_cy, L.btn_r, orng, bg, 'O', olbl);
    disc_sphere(L.x_cx, L.x_cy, L.btn_r, xrng, bg, 'X', xlbl);
    /* MENU: outline pill (design fill=none) + muted centred label */
    int mt = 1 + L.menu_h / 16;
    rrect_outline(L.menu_cx - L.menu_w / 2, L.menu_cy - L.menu_h / 2, L.menu_w, L.menu_h,
                  L.menu_h / 2, mt, menu, bg);
    text(L.menu_cx, L.menu_cy, "MENU", mlbl, bg, L.lbl > 1 ? L.lbl - 1 : 1);
    ESP_LOGI(TAG, "deck drawn (portable): dpad(%d,%d) O(%d,%d) X(%d,%d) menu(%d,%d) r=%d",
             L.dpad_cx, L.dpad_cy, L.o_cx, L.o_cy, L.x_cx, L.x_cy, L.menu_cx, L.menu_cy, L.btn_r);
}

esp_err_t input_init(void) {
    s_ok = (board_touch_init() == ESP_OK);
    if (s_ok) { layout_init(); draw_deck(); }   /* paint the on-screen controls once (static; game drawn above) */
    ESP_LOGI(TAG, "touch backend %s (tap the deck: d-pad / O / X / menu)", s_ok ? "ready" : "unavailable");
    return s_ok ? ESP_OK : ESP_FAIL;
}

uint8_t input_poll(void) {
    static bool was = false;
    if (!s_ok) return 0;
    int xs[2], ys[2];
    int n = board_touch_read(xs, ys, 2);
    uint8_t held = 0;
    uint8_t bit[2] = {0, 0};
    for (int i = 0; i < n; ++i) { bit[i] = point_to_button(xs[i], ys[i]); held |= bit[i]; }
    if (n > 0 && !was) {                 /* log each touch-DOWN once (calibration aid), not every frame */
        for (int i = 0; i < n; ++i)
            ESP_LOGI(TAG, "touch (%d,%d) -> 0x%02x", xs[i], ys[i], bit[i]);
    }
    was = (n > 0);
    return held;
}

void        input_set_frame(uint32_t fc) { (void)fc; }   /* no-op: only the scheduled backend uses the fc */
const char *input_backend_name(void) { return "touch"; }

/* no-op: only the scheduled backend tracks deadline misses (report zeros so main.cpp can stream unconditionally) */
void input_sched_stats(uint32_t *fed, uint32_t *miss, uint32_t *applied) {
    if (fed) *fed = 0;
    if (miss) *miss = 0;
    if (applied) *applied = 0;
}
