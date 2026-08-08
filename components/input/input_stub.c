/* input_stub.c — the default backend: no input. KDown=KHeld=0, so the pause menu never opens and a
 * cart runs untouched. Keeps a fresh `make build` (no `INPUT_BACKEND` cached) behaving as before the
 * input seam existed. (The value persists in CMakeCache once set — see the component CMakeLists.)
 * See docs/runtime/pico-e32-fake08-input.md. */
#include "input.h"

esp_err_t   input_init(void)         { return ESP_OK; }
/* Calls input_exit_check even though the mask is always 0: every backend routes through it, so a backend
 * that later grows real buttons inherits the exit gesture instead of silently lacking it (IN-6). */
uint8_t     input_poll(void)         { input_exit_check(0); return 0; }
void        input_set_frame(uint32_t fc) { (void)fc; }   /* no-op: only the scheduled backend uses the fc */
const char *input_backend_name(void) { return "stub"; }

/* no-op: only the scheduled backend tracks deadline misses (report zeros so main.cpp can stream unconditionally) */
void input_sched_stats(uint32_t *fed, uint32_t *miss, uint32_t *applied) {
    if (fed) *fed = 0; if (miss) *miss = 0; if (applied) *applied = 0;
}
