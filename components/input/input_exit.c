/* input_exit — hold MENU in a running cart to get back to the launcher (IN-6).
 *
 * Why this lives in the input layer rather than the app or the Host:
 *   - app_main is blocked inside Vm::GameLoop(), which by design never returns, so there is nowhere in the app
 *     for a per-frame check to live;
 *   - a background watcher task cannot poll for the gesture, because input_poll() is DESTRUCTIVE on the serial
 *     backend (it drains the UART / USB-JTAG buffer and decrements hold counters) — a second caller would eat
 *     input the VM never sees;
 *   - ESP32Host::scanInput() is the architecturally tidier home, but it lives in the fake-08 submodule.
 * So each backend calls this from inside its own input_poll(), which is the one per-frame path we own.
 *
 * Restart IS the return path: the launcher is ~1.8 s from reset with the SD already mounted and the radio off
 * (WC-5), so rebooting is cheaper and far safer than unwinding a loop documented never to return. Nothing is
 * lost that was not already lost — PICO-8 carts keep persistent state via cartdata, which is flushed on write.
 *
 * A HOLD, not a tap: a tap must keep meaning PICO-8's own pause (Vm::togglePauseMenu). */
#include <stdbool.h>
#include "input.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "input.exit";

/* Wall-clock, not a poll count: the poll cadence is the game loop's and varies by build (a telemetry or
 * measure-fps loop does not run at 60 Hz), so counting polls would silently change the gesture's length.
 * 1.2 s — long enough not to fire on a deliberate pause or a thumb resting on the deck, short enough not to
 * feel broken while you hold it. */
#ifndef INPUT_EXIT_HOLD_MS
#define INPUT_EXIT_HOLD_MS 1200
#endif

/* Off until the app says otherwise: in the LAUNCHER, MENU is not an exit (its screens leave with X, and a
 * resting thumb on the deck should not reboot the device). The app arms this immediately before handing
 * control to the VM, so the gesture only exists while a cart is actually running. */
static bool s_armed = false;

void input_exit_enable(bool on) { s_armed = on; }

void input_exit_check(uint8_t held) {
#if INPUT_EXIT_HOLD_MS > 0
    static int64_t since = 0;                  /* us at which the current unbroken hold started; 0 = not held */
    if (!s_armed || !(held & INPUT_PAUSE)) { since = 0; return; }
    int64_t now = esp_timer_get_time();
    if (since == 0) { since = now; return; }
    if (now - since < (int64_t)INPUT_EXIT_HOLD_MS * 1000) return;
    since = 0;
    ESP_LOGW(TAG, "MENU held %d ms — returning to the launcher (restart)", INPUT_EXIT_HOLD_MS);
    esp_restart();
#else
    (void)held;
#endif
}
