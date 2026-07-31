/* input_serial.c — serial input backend: bytes over the console link become PICO-8 buttons.
 *
 * This is the hardware-in-the-loop path. An automated bench can't press a button or touch the panel,
 * but it can write bytes to the board's console — so the whole scanInput -> btn/btnp -> cart path becomes
 * verifiable over the wire. See docs/runtime/pico-e32-fake08-input.md.
 *
 * Protocol: one byte per key (case-insensitive), held for HOLD_FRAMES then auto-released, so a single
 * byte is a tap and repeated bytes hold. l/r/u/d = dpad, z or o = O, x = X, p = pause.
 *
 * Transport is the board's console link, chosen at build time by the console config so one file serves
 * both boards:
 *   - CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG (ESP32-P4 Guition board): native USB-Serial-JTAG.
 *   - else (ESP32-S3 Makerfabs board): UART0 (the CP2104 bridge).
 * Coexistence: the same link also carries the console log TX; we install the RX driver and read raw
 * bytes while ESP_LOG keeps writing on TX. */
#include "input.h"

#include <stdbool.h>
#include "esp_log.h"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#define IN_RX_BUF   256
#else
#include "driver/uart.h"
#define IN_UART     UART_NUM_0
#define IN_RX_BUF   256
#endif
/* game-update frames a byte stays held (auto-release). Default 6 (~200 ms at 30 fps) suits a human
 * typing single keys; an automated frame-synced driver overrides it to 1 (`-D INPUT_HOLD_FRAMES=1`)
 * for frame-exact control (each byte = exactly one held frame; re-send every frame to keep held). */
#ifndef INPUT_HOLD_FRAMES
#define INPUT_HOLD_FRAMES 6
#endif
#define HOLD_FRAMES INPUT_HOLD_FRAMES

/* Board seam (resolved at the final app link, like board_lcd_*): paint the on-screen control deck. The
 * serial backend takes its input over the wire, but it draws the SAME static deck as the touch backend so
 * the play-test build shows the identical button layout as the shipped touch build — the panel just isn't
 * read for input here. A board with no deck omits this hook, so an INPUT_BACKEND=serial build link-fails
 * there (the intended signal); both current boards define it. */
extern void board_draw_touch_deck(void);

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
    esp_err_t r = ESP_OK;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    /* Native USB-Serial-JTAG: install the interrupt-driven driver so we can read host bytes; ESP_LOG keeps
     * writing on TX. A second install returns ESP_ERR_INVALID_STATE (already up) — treat that as fine. */
    usb_serial_jtag_driver_config_t cfg = { .tx_buffer_size = 256, .rx_buffer_size = IN_RX_BUF };
    r = usb_serial_jtag_driver_install(&cfg);
    const char *link = "USB-Serial-JTAG";
#else
    /* UART0: the console — or the app's dev-only TELEMETRY_HOST_CFG startup read — may already own the RX
     * driver. Install it only if it isn't already: a second install can return ESP_FAIL (not just
     * ESP_ERR_INVALID_STATE), which would wrongly disable input. If it's up, just use it. */
    if (!uart_is_driver_installed(IN_UART)) {
        r = uart_driver_install(IN_UART, IN_RX_BUF, 0, 0, NULL, 0);
    }
    const char *link = "UART0";
#endif
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "serial input driver install: %s - unavailable", esp_err_to_name(r));
        s_ok = false;
        return r;
    }
    s_ok = true;
    board_draw_touch_deck();   /* paint the on-screen control deck once (static overlay) — same layout as the
                                * touch build; input still comes over the wire, the panel just isn't read. */
    ESP_LOGI(TAG, "serial input on %s: l/r/u/d dir, z=O x=X p=pause (tap holds %d frames)", link, HOLD_FRAMES);
    return ESP_OK;
}

uint8_t input_poll(void) {
    if (s_ok) {
        unsigned char buf[32];
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), 0);  /* non-blocking */
#else
        int n = uart_read_bytes(IN_UART, buf, sizeof(buf), 0);    /* non-blocking */
#endif
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

/* no-op: only the scheduled backend tracks deadline misses (report zeros so main.cpp can stream unconditionally) */
void input_sched_stats(uint32_t *fed, uint32_t *miss, uint32_t *applied) {
    if (fed) *fed = 0;
    if (miss) *miss = 0;
    if (applied) *applied = 0;
}
