/* input_scheduled.c — fc-SCHEDULED serial input backend (frame-exact closed-loop over telemetry).
 *
 * The problem: the plain serial backend applies a byte on whichever input_poll() happens to catch it — a
 * ~1-frame JITTERY latency that is fatal to frame-precise carts (a Celeste dash one frame late launches from
 * the wrong pixel and dies; measured on the sim — see docs/runtime/pico-e32-fake08-input.md IN-5). The jitter
 * is the phase mismatch between the device's fixed frame clock and the host's async loop, NOT the telemetry
 * cost. This backend removes it without abandoning telemetry:
 *
 *   * the host tags each command with a TARGET FRAME (the telemetry `fc` it reads, + a lead k), and
 *   * the device applies it when its frame clock REACHES that fc — so host jitter only has to BEAT A DEADLINE
 *     (arrive before the target frame); the apply instant is deterministic.
 *
 * Two cores (ESP32-S3): a dedicated fc_rx task on APP_CPU (core 1, idle in this app) drains the wire the
 * instant bytes land and pushes parsed commands into a lock-free SPSC ring; the game task (core 0) drains the
 * ring in input_poll(), holds commands in a small table, and applies each when the frame clock reaches it.
 * The frame clock is handed in by main.cpp via input_set_frame() before each Step (the fc this frame emits).
 *
 * Wire format (host -> device), 8 bytes, little-endian, self-delimiting + checksummed — mirrors
 * test/playtest/fc_sched.py exactly:
 *     0xA6 | target_fc:u32 | mask:u8 | hold:u8 | csum:u8      (csum = XOR of bytes 0..6)
 * mask (INPUT_* bits) is held for game-frames [target_fc, target_fc + 2*hold); a one-frame dash is hold=1.
 *
 * Dev/HITL only, pairs with TELEMETRY (the host needs the streamed fc). Coexists with the console log on
 * UART0 (log = TX, commands = RX); non-command bytes are skipped for free by the framing. */
#include "input.h"

#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define IN_UART    UART_NUM_0
#define IN_RX_BUF  256
#define SYNC       0xA6
#define PKT_LEN    8

#if CONFIG_FREERTOS_UNICORE
#define FC_RX_CORE 0
#else
#define FC_RX_CORE 1              /* APP_CPU — idle in this app; keep serial servicing off the game task */
#endif

#define RING_SZ    32            /* SPSC ring capacity (power of two) — commands in flight core1 -> core0 */
#define TABLE_SZ   16            /* pending scheduled commands on core0 (>= worst-case in-flight) */

static const char *TAG = "input.sched";

typedef struct { uint32_t fc; uint8_t mask; uint8_t hold; } sched_cmd_t;

/* --- lock-free SPSC ring: fc_rx (core 1) writes, input_poll (core 0) reads --------------------------------*/
static sched_cmd_t     s_ring[RING_SZ];
static _Atomic uint32_t s_head = 0;      /* producer index (monotonic; mask on use) */
static _Atomic uint32_t s_tail = 0;      /* consumer index */

static bool ring_push(const sched_cmd_t *c) {         /* core 1 only */
    uint32_t h = atomic_load_explicit(&s_head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&s_tail, memory_order_acquire);
    if (h - t >= RING_SZ) return false;               /* full -> drop */
    s_ring[h & (RING_SZ - 1)] = *c;
    atomic_store_explicit(&s_head, h + 1, memory_order_release);
    return true;
}
static bool ring_pop(sched_cmd_t *c) {                /* core 0 only */
    uint32_t t = atomic_load_explicit(&s_tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&s_head, memory_order_acquire);
    if (t == h) return false;                         /* empty */
    *c = s_ring[t & (RING_SZ - 1)];
    atomic_store_explicit(&s_tail, t + 1, memory_order_release);
    return true;
}

/* --- core-0 state: the pending table + the frame clock + counters ----------------------------------------*/
static sched_cmd_t     s_table[TABLE_SZ];
static int             s_ntab;
static volatile uint32_t s_apply_fc;     /* the fc of the frame being produced (set by main pre-Step) */
static uint32_t s_fed, s_miss, s_applied, s_overflow;   /* core 0 only (poll + insert) */
static uint32_t s_ring_drop;                            /* core 1: commands dropped on a full ring (diagnostic) */

static bool s_ok;
static TaskHandle_t s_rx_task;

/* --- fc_rx task (core 1): drain UART -> parse frames -> push to the ring ----------------------------------*/
static uint8_t s_acc[PKT_LEN];
static int     s_acclen;

static void feed_bytes(const uint8_t *b, int n) {
    for (int i = 0; i < n; ++i) {
        if (s_acclen == 0 && b[i] != SYNC) continue;   /* scan for the sync byte */
        s_acc[s_acclen++] = b[i];
        if (s_acclen < PKT_LEN) continue;
        uint8_t csum = 0;
        for (int j = 0; j < PKT_LEN - 1; ++j) csum ^= s_acc[j];
        if (csum == s_acc[PKT_LEN - 1]) {
            sched_cmd_t c;
            memcpy(&c.fc, &s_acc[1], 4);                /* u32 LE (ESP32 is little-endian) */
            c.mask = s_acc[5];
            c.hold = s_acc[6];
            if (!ring_push(&c)) s_ring_drop++;         /* full ring -> dropped + counted (folded into stats' miss) */
            s_acclen = 0;
        } else {
            /* bad frame: drop the leading byte and resync to the next SYNC in what we already have */
            memmove(s_acc, s_acc + 1, PKT_LEN - 1);
            s_acclen = PKT_LEN - 1;
            while (s_acclen > 0 && s_acc[0] != SYNC) {
                memmove(s_acc, s_acc + 1, --s_acclen);
            }
        }
    }
}

static void fc_rx_task(void *arg) {
    (void)arg;
    uint8_t buf[64];
    for (;;) {
        /* Block for the FIRST byte only (uart_read_bytes with a multi-byte length + portMAX_DELAY would block
         * until the whole buffer fills — fatal for a trickled 8-byte protocol), then drain whatever else is
         * already buffered non-blocking. Matches input_serial.c's non-blocking read. */
        int n = uart_read_bytes(IN_UART, buf, 1, portMAX_DELAY);
        if (n > 0) {
            int m = uart_read_bytes(IN_UART, buf + 1, sizeof(buf) - 1, 0);
            if (m > 0) n += m;
            feed_bytes(buf, n);
        } else {
            vTaskDelay(pdMS_TO_TICKS(2));               /* driver error/closed: back off, don't tight-loop */
        }
    }
}

/* --- the seam --------------------------------------------------------------------------------------------*/
esp_err_t input_init(void) {
    esp_err_t r = ESP_OK;
    if (!uart_is_driver_installed(IN_UART)) {          /* main's CFG read / the serial backend may own it */
        r = uart_driver_install(IN_UART, IN_RX_BUF, 0, 0, NULL, 0);
    }
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "uart_driver_install: %s - fc-scheduled input unavailable", esp_err_to_name(r));
        s_ok = false;
        return r;
    }
    s_ok = true;
    /* pin the UART service to APP_CPU so it never perturbs the game task's frame timing */
    if (xTaskCreatePinnedToCore(fc_rx_task, "fc_rx", 3072, NULL, 12, &s_rx_task, FC_RX_CORE) != pdPASS) {
        ESP_LOGW(TAG, "fc_rx task create failed - fc-scheduled input unavailable");
        s_ok = false;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "fc-scheduled input on UART0 (fc_rx on core %d): 8B cmd 0xA6|fc|mask|hold|csum, apply at fc",
             FC_RX_CORE);
    return ESP_OK;
}

/* main.cpp calls this before each Step with the fc THIS frame will emit (GetFrameCount()+2). Core 0 only. */
void input_set_frame(uint32_t fc) { s_apply_fc = fc; }

uint8_t input_poll(void) {
    if (!s_ok) return 0;
    uint32_t G = s_apply_fc;

    sched_cmd_t c;                                     /* drain the latch into the pending table */
    while (ring_pop(&c)) {
        s_fed++;
        if (c.fc < G) { s_miss++; continue; }          /* target frame already produced (G>fc) -> a deadline miss */
        if (s_ntab < TABLE_SZ) s_table[s_ntab++] = c;  /* c.fc == G is on time: inserted, then applied below */
        else s_overflow++;
    }

    uint8_t held = 0;                                  /* apply every command whose window covers this frame */
    for (int i = 0; i < s_ntab; ) {
        sched_cmd_t *e = &s_table[i];
        uint32_t end = e->fc + 2u * e->hold;
        if (e->fc <= G && G < end) {
            held |= e->mask;
            if (e->fc == G) s_applied++;               /* count each command once, at its first active frame */
            ++i;
        } else if (G >= end) {
            s_table[i] = s_table[--s_ntab];            /* expired -> swap-remove */
        } else {
            ++i;                                       /* still in the future */
        }
    }
    return held;
}

const char *input_backend_name(void) { return "scheduled"; }

/* Deadline miss-rate accounting for telemetry / logging: fed = valid commands seen, miss = arrived too late,
 * applied = reached their active window. miss/fed is the on-device number the design turns on. */
void input_sched_stats(uint32_t *fed, uint32_t *miss, uint32_t *applied) {
    if (fed)     *fed = s_fed + s_ring_drop;                 /* ring-drops are commands that never made it in */
    if (miss)    *miss = s_miss + s_ring_drop + s_overflow;  /* every command that failed to apply on time */
    if (applied) *applied = s_applied;
}
