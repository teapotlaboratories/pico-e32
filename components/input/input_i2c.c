/* input_i2c.c — I²C GPIO-expander button backend (IN-3). SKELETON.
 *
 * Plan: read physical game buttons from an I²C GPIO expander (at an address != 0x38, which the touch
 * controller uses) and OR the bits into the mask. This is the eventual handheld input — parts-blocked
 * (no expander/buttons yet). Kept so the compile-time switch is complete and `-D INPUT_BACKEND=i2c`
 * builds. Board wiring will come via board_input_config(). See docs/runtime/pico-e32-fake08-input.md. */
#include "input.h"
#include "esp_log.h"

static const char *TAG = "input.i2c";

esp_err_t input_init(void) {
    ESP_LOGW(TAG, "I2C expander input backend not implemented yet (IN-3, parts-blocked) - no input");
    return ESP_OK;
}
/* The exit-gesture hook is already wired (IN-6) so that implementing IN-3 here is only about producing the
 * mask: the hold-MENU-to-launcher path comes along for free rather than being forgotten on real hardware. */
uint8_t     input_poll(void)         { uint8_t m = 0; input_exit_check(m); return m; }
void        input_set_frame(uint32_t fc) { (void)fc; }   /* no-op: only the scheduled backend uses the fc */
const char *input_backend_name(void) { return "i2c(stub)"; }

/* no-op: only the scheduled backend tracks deadline misses (report zeros so main.cpp can stream unconditionally) */
void input_sched_stats(uint32_t *fed, uint32_t *miss, uint32_t *applied) {
    if (fed) *fed = 0; if (miss) *miss = 0; if (applied) *applied = 0;
}
