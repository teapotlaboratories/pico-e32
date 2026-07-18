/* input_stub.c — the default backend: no input. KDown=KHeld=0, so the pause menu never opens and a
 * cart runs untouched. Keeps a plain `make build` (no INPUT_BACKEND) behaving exactly as before the
 * input seam existed. See docs/runtime/pico-e32-fake08-input.md. */
#include "input.h"

esp_err_t   input_init(void)         { return ESP_OK; }
uint8_t     input_poll(void)         { return 0; }
const char *input_backend_name(void) { return "stub"; }
