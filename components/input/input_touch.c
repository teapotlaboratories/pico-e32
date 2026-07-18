/* input_touch.c — FT6236 capacitive-touch backend (IN-2). SKELETON.
 *
 * Plan: poll the on-board FT6236 over I²C (0x38, SDA38/SCL39 on makerfabs-ili9488-r1), map screen zones
 * (an on-screen d-pad + O/X) to the button mask. The board will supply its touch wiring via a
 * board_input_config() (I²C pins + address), symmetric with board_sd_config. On-board, no parts.
 * Not implemented yet — kept so the compile-time switch is complete and `-D INPUT_BACKEND=touch` builds.
 * See docs/runtime/pico-e32-fake08-input.md. */
#include "input.h"
#include "esp_log.h"

static const char *TAG = "input.touch";

esp_err_t input_init(void) {
    ESP_LOGW(TAG, "FT6236 touch backend not implemented yet (IN-2) - no input this build");
    return ESP_OK;
}
uint8_t     input_poll(void)         { return 0; }
const char *input_backend_name(void) { return "touch(stub)"; }
