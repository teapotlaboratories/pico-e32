/* input.h — compile-time-selectable game-input seam for the fake-08 Host (pico-e32).
 *
 * fake-08's Host::scanInput() reduces to a single call here: input_poll() returns the currently-HELD
 * PICO-8 button mask, and the Host computes the pressed-this-frame edge itself. Exactly ONE backend is
 * compiled per build (INPUT_BACKEND: stub | serial | touch | i2c) — see the component CMakeLists and
 * docs/runtime/pico-e32-fake08-input.md. Bits match fake-08's P8_KEY_* order (hostVmShared.h). */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    INPUT_LEFT  = 1 << 0,
    INPUT_RIGHT = 1 << 1,
    INPUT_UP    = 1 << 2,
    INPUT_DOWN  = 1 << 3,
    INPUT_O     = 1 << 4,   /* the O / Z button */
    INPUT_X     = 1 << 5,   /* the X button     */
    INPUT_PAUSE = 1 << 6,
};

/* Bring the selected backend up once (called at the first scanInput). ESP_OK on success, or on a
 * degraded-but-usable state; a hard failure returns an error and input_poll() then reports nothing. */
esp_err_t input_init(void);

/* The currently-held button mask (INPUT_* bits). Called once per frame. Never blocks. */
uint8_t input_poll(void);

/* The compiled backend's name, for the boot log. */
const char *input_backend_name(void);

#ifdef __cplusplus
}
#endif
