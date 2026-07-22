/* input_serial.c — serial (UART0) input backend: bytes over the console UART become PICO-8 buttons.
 *
 * This is the hardware-in-the-loop path. An automated bench can't press a button or touch the panel,
 * but it can write bytes to the board's CP2104 console UART — so the whole scanInput -> btn/btnp -> cart
 * path becomes verifiable over the wire. See docs/runtime/pico-e32-fake08-input.md.
 *
 * Protocol: one byte per key (case-insensitive), held for HOLD_FRAMES then auto-released, so a single
 * byte is a tap and repeated bytes hold. l/r/u/d = dpad, z or o = O, x = X, p = pause.
 *
 * Coexistence: UART0 is also the console log TX. We install the RX driver and read raw bytes; ESP_LOG
 * keeps writing on TX. If a build ever shows the console fighting this, move to USB-Serial-JTAG. */
#include "input.h"

#include <stdbool.h>
#include "driver/uart.h"
#include "esp_log.h"

#define IN_UART     UART_NUM_0
#define IN_RX_BUF   256
/* game-update frames a byte stays held (auto-release). Default 6 (~200 ms at 30 fps) suits a human
 * typing single keys; an automated frame-synced driver overrides it to 1 (`-D INPUT_HOLD_FRAMES=1`)
 * for frame-exact control (each byte = exactly one held frame; re-send every frame to keep held). */
#ifndef INPUT_HOLD_FRAMES
#define INPUT_HOLD_FRAMES 6
#endif
#define HOLD_FRAMES INPUT_HOLD_FRAMES

static const char *TAG = "input.serial";
static bool    s_ok;
static uint8_t s_hold[7];    /* per-button remaining hold frames, bits 0..6 */

static int key_to_bit(unsigned char c) {
    switch (c) {
        case 'l': case 'L': return 0;   /* LEFT  */
        case 'r': case 'R': return 1;   /* RIGHT */
        case 'u': case 'U': return 2;   /* UP    */
        case 'd': case 'D': return 3;   /* DOWN  */
        case 'z': case 'Z':
        case 'o': case 'O': return 4;   /* O (Z key) */
        case 'x': case 'X': return 5;   /* X */
        case 'p': case 'P': return 6;   /* PAUSE */
        default: return -1;
    }
}

esp_err_t input_init(void) {
    /* The console — or the app's dev-only TELEMETRY_HOST_CFG startup read — may already own the UART0 RX
     * driver. Install it only if it isn't already: a second install can return ESP_FAIL (not just
     * ESP_ERR_INVALID_STATE), which would wrongly disable input. If it's up, just use it. */
    esp_err_t r = ESP_OK;
    if (!uart_is_driver_installed(IN_UART)) {
        r = uart_driver_install(IN_UART, IN_RX_BUF, 0, 0, NULL, 0);
    }
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "uart_driver_install: %s - serial input unavailable", esp_err_to_name(r));
        s_ok = false;
        return r;
    }
    s_ok = true;
    ESP_LOGI(TAG, "serial input on UART0 (inherits the console baud): l/r/u/d dir, z=O x=X p=pause "
             "(tap holds %d frames)", HOLD_FRAMES);
    return ESP_OK;
}

uint8_t input_poll(void) {
    if (s_ok) {
        unsigned char buf[32];
        int n = uart_read_bytes(IN_UART, buf, sizeof(buf), 0);   /* non-blocking */
        for (int i = 0; i < n; ++i) {
            int b = key_to_bit(buf[i]);
            if (b >= 0) {
                s_hold[b] = HOLD_FRAMES;
                ESP_LOGI(TAG, "%c", buf[i]);   /* receive->map visible in the log, no camera needed */
            }
        }
    }
    uint8_t held = 0;
    for (int b = 0; b < 7; ++b) {
        if (s_hold[b] > 0) { s_hold[b]--; held |= (uint8_t)(1u << b); }
    }
    return held;
}

void        input_set_frame(uint32_t fc) { (void)fc; }   /* no-op: only the scheduled backend uses the fc */
const char *input_backend_name(void) { return "serial"; }
