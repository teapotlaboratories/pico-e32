/* input_touch.c — capacitive-touch input backend (IN-2). Board-agnostic GLUE only.
 *
 * The board owns everything screen-specific: the touch controller + this panel's orientation
 * (board_touch_init / board_touch_read — points already in DISPLAY coordinates), AND the on-screen
 * control deck — both its LAYOUT and its RENDERING (board_draw_touch_deck), plus the touch->button
 * MAPPING (board_touch_hittest). Each board draws with primitives suited to its own display (the S3 uses
 * LovyanGFX vector primitives; the P4 rasterises into its framebuffer), so screen geometry and per-panel
 * display quirks never leak into this shared layer. This file just brings the backend up and folds
 * hit-tested points into the held mask. See docs/runtime/pico-e32-fake08-input.md (IN-2). */
#include "input.h"
#include "esp_log.h"
#include <stdbool.h>
#include <stdint.h>

/* Board seam — resolved at the final app link (like board_lcd_*). A board with no touch omits these
 * board_touch_* / board_draw_touch_deck hooks, so an INPUT_BACKEND=touch build link-fails there: the
 * intended signal. */
extern esp_err_t board_touch_init(void);
extern int       board_touch_read(int *xs, int *ys, int max);
extern void      board_draw_touch_deck(void);        /* paint the deck once; board owns layout + rendering */
extern uint8_t   board_touch_hittest(int x, int y);  /* touch (display coords) -> INPUT_* bit, 0 = none    */

static const char *TAG = "input.touch";
static bool s_ok;

esp_err_t input_init(void) {
    s_ok = (board_touch_init() == ESP_OK);
    if (s_ok) board_draw_touch_deck();               /* paint the on-screen controls once (static overlay) */
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
    for (int i = 0; i < n; ++i) { bit[i] = board_touch_hittest(xs[i], ys[i]); held |= bit[i]; }
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
