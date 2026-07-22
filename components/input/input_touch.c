/* input_touch.c — FT6236 capacitive-touch input backend (IN-2).
 *
 * The board owns the FT6236 hardware and this panel's orientation (board_touch_init / board_touch_read,
 * which returns points already in display coordinates). This file owns only the MAPPING: a touch point ->
 * an on-screen control zone -> a PICO-8 button bit, OR'd into the held mask. The FT6236 reports two touch
 * points, so a direction and an action button register at once (move + jump).
 *
 * Layout: see docs/runtime/pico-e32-fake08-touch-ui.html (the approved control deck). Screen is the top
 * 256x256; the control deck is the bottom band. See docs/runtime/pico-e32-fake08-input.md (IN-2). */
#include "input.h"
#include "esp_log.h"
#include <stdbool.h>

/* Provided by the board (boards/<board>/board.cpp), resolved at the final app link — like board_lcd_*.
 * A board with no touch omits these, so an INPUT_BACKEND=touch build link-fails there: the intended signal. */
extern esp_err_t board_touch_init(void);
extern int       board_touch_read(int *xs, int *ys, int max);
extern void      board_draw_touch_deck(void);

static const char *TAG = "input.touch";
static bool s_ok;

/* Control-deck zones — panel 320x480, from the mockup. */
#define DPAD_CX    92
#define DPAD_CY    376
#define DPAD_REACH 74      /* half-extent of the d-pad cross bounding box */
#define DPAD_DEAD  16      /* dead hub radius */
#define O_CX 212
#define O_CY 414
#define X_CX 272
#define X_CY 352
#define BTN_R 36           /* button hit radius (a touch larger than the drawn r=31) */
#define MENU_X0 124
#define MENU_X1 196
#define MENU_Y0 266
#define MENU_Y1 300

static inline int sq(int v) { return v * v; }

/* One touch point (display coords) -> a single button bit (0 if it hits nothing). */
static uint8_t point_to_button(int x, int y) {
    int dx = x - DPAD_CX, dy = y - DPAD_CY;
    if (dx > -DPAD_REACH && dx < DPAD_REACH && dy > -DPAD_REACH && dy < DPAD_REACH) {
        if (sq(dx) + sq(dy) < sq(DPAD_DEAD)) return 0;             /* dead hub */
        int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;       /* dominant axis picks the direction */
        if (ax > ay) return dx < 0 ? INPUT_LEFT : INPUT_RIGHT;
        return dy < 0 ? INPUT_UP : INPUT_DOWN;                     /* smaller y = up */
    }
    if (sq(x - O_CX) + sq(y - O_CY) < sq(BTN_R)) return INPUT_O;
    if (sq(x - X_CX) + sq(y - X_CY) < sq(BTN_R)) return INPUT_X;
    if (x >= MENU_X0 && x <= MENU_X1 && y >= MENU_Y0 && y <= MENU_Y1) return INPUT_PAUSE;
    return 0;
}

esp_err_t input_init(void) {
    s_ok = (board_touch_init() == ESP_OK);
    if (s_ok) board_draw_touch_deck();   /* paint the on-screen controls once (static; game is drawn above) */
    ESP_LOGI(TAG, "FT6236 touch backend %s (tap the deck: d-pad / O / X / menu)",
             s_ok ? "ready" : "unavailable");
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
