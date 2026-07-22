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
uint8_t     input_poll(void)         { return 0; }
void        input_set_frame(uint32_t fc) { (void)fc; }   /* no-op: only the scheduled backend uses the fc */
const char *input_backend_name(void) { return "i2c(stub)"; }
