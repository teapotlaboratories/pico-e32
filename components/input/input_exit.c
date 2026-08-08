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
 * (WC-5), so rebooting is cheaper and far safer than unwinding a loop documented never to return.
 *
 * What that costs, stated plainly: cart save state IS discarded. fake-08's dset() only pokes into RAM at
 * 0x5e00 (Vm::vm_dset); the blob reaches disk from CloseCart(), vm_load() and a cartdata() key change only —
 * none of which run on this path. So anything a cart saved since it loaded is gone. A real CONTINUE / EXIT
 * overlay (upstream's `//todo` in vm.cpp) is what would fix that; this gesture is the cheap way out until then.
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

/* Wall-clock alone is not enough: it measures the time between two polls that happened to see MENU held, and
 * says nothing about what happened in between. A single serial byte holds INPUT_PAUSE for INPUT_HOLD_FRAMES
 * polls, so a *tap* followed by any long stall — carousel_fb_dump()'s multi-second serial transfer, a GC
 * pause, a slow SD read — would come back to a poll >1.2 s after the first and reboot from a tap. So a hold
 * is only unbroken if consecutive held polls stay close together; a bigger gap restarts the measurement. */
#ifndef INPUT_EXIT_MAX_GAP_MS
#define INPUT_EXIT_MAX_GAP_MS 250
#endif

/* Off until the app says otherwise: in the LAUNCHER, MENU is not an exit (its screens leave with X, and a
 * resting thumb on the deck should not reboot the device). The app arms this immediately before handing
 * control to the VM, so the gesture only exists while a cart is actually running. */
static bool s_armed = false;

void input_exit_enable(bool on) { s_armed = on; }

void input_exit_check(uint8_t held) {
#if INPUT_EXIT_HOLD_MS > 0
    static int64_t since = 0;                  /* us at which the current run started; 0 = no run in progress */
    static int64_t last  = 0;                  /* us of the most recent poll that actually saw MENU held */
    if (!s_armed) { since = 0; return; }
    int64_t now = esp_timer_get_time();
    const int64_t gap = (int64_t)INPUT_EXIT_MAX_GAP_MS * 1000;

    /* A poll that does not report MENU is NOT proof the user let go. On the P4's shipped touch backend,
     * board_touch_read() returns 0 on any GT911 I2C hiccup and — routinely — on every poll where the
     * controller's buffer-ready bit is clear because it has not posted a new frame since the last read. At a
     * 60 Hz poll that happens constantly, so zeroing the run on one empty poll made the gesture essentially
     * unfirable on the very build that ships. Only silence for longer than the gap ends a run; that is the
     * same tolerance already granted between two held polls, applied consistently.
     *
     * The residual: taps spaced closer together than the gap keep one run alive, so mashing MENU at ~5/s for
     * a solid 1.2 s would trip the exit. That is a deliberate act, not a slip, and it is the price of the
     * dropout tolerance the shipped sensor needs. */
    if (!(held & INPUT_PAUSE)) {
        if (since != 0 && now - last > gap) since = 0;
        return;
    }
    if (since == 0 || now - last > gap) {
        since = now; last = now; return;       /* first held poll, or the run was broken by a real stall */
    }
    last = now;
    if (now - since < (int64_t)INPUT_EXIT_HOLD_MS * 1000) return;
    since = 0;
    ESP_LOGW(TAG, "MENU held %d ms — returning to the launcher (restart)", INPUT_EXIT_HOLD_MS);
    esp_restart();
#else
    (void)held;
#endif
}
