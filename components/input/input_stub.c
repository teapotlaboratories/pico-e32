/* input_stub.c — the default backend: no input. KDown=KHeld=0, so the pause menu never opens and a
 * cart runs untouched. Keeps a fresh `make build` (no `INPUT_BACKEND` cached) behaving as before the
 * input seam existed. (The value persists in CMakeCache once set — see the component CMakeLists.)
 * See docs/runtime/pico-e32-fake08-input.md. */
#include "input.h"

esp_err_t   input_init(void)         { return ESP_OK; }
uint8_t     input_poll(void)         { return 0; }
void        input_set_frame(uint32_t fc) { (void)fc; }   /* no-op: only the scheduled backend uses the fc */
const char *input_backend_name(void) { return "stub"; }
