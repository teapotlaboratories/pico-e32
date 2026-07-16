/* Phase-0 Track A — ILI9488 scaled PICO-8 blit + FPS (Gate #1).
 *
 * Board-agnostic: the panel wiring comes from boards/<BOARD>/board_pins.h (ILI9488_PINS),
 * selected by BOARD at build time. Target board today: an ILI9488 320x480 parallel panel,
 * 16-bit Intel-8080 parallel. The display driver now lives in the reusable
 * components/ili9488 module; this file is just the Gate #1 test harness (board
 * pin map + PICO-8 palette test image + 2x scale + FPS loop).
 *
 *   A1: show 16 PICO-8 palette bars (a 128x128 indexed test image) -> proves
 *       the bus, pins, orientation and colour order.
 *   A2: palette-expand + integer 2x scale to 256x256, centred, redraw in a loop,
 *       and print the sustained FPS over UART.
 *
 * GATE #1: >= 30 fps sustained for the 256x256 scaled full-refresh.
 *
 * Orientation and colour are NOT adjusted here. Orientation is a board fact
 * (ILI9488_PINS -> .mirror_y in boards/<BOARD>/board_pins.h); .swap_color_bytes decides
 * only WHICH SIDE swaps the RGB565 bytes, not whether the colours come out right, so
 * flipping it to chase a colour problem just costs fps. The 2026-07-14 "288 fps" that used
 * to be quoted here was void -- it clocked DMA into unconnected pins on the wrong pin map.
 * See docs/worklog/2026-07-16-yflip-and-gate1-fps.md. */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ili9488.h"
#include "board_pins.h"

static const char *TAG = "trackA";

#define LCD_H_RES 320
#define LCD_V_RES 480

/* PICO-8 native + 2x integer scale */
#define P8    128
#define SCALE 2
#define OUT   (P8 * SCALE)                 /* 256 */
#define OX    ((LCD_H_RES - OUT) / 2)      /* 32  */
#define OY    ((LCD_V_RES - OUT) / 2)      /* 112 */

/* PICO-8 16-colour palette as RGB888 */
static const uint8_t PAL888[16][3] = {
  {0,0,0},{29,43,83},{126,37,83},{0,135,81},{171,82,54},{95,87,79},{194,195,199},{255,241,232},
  {255,0,77},{255,163,0},{255,236,39},{0,228,54},{41,173,255},{131,118,156},{255,119,168},{255,204,170},
};
static uint16_t pal565[16];

void app_main(void) {
    ili9488_config_t cfg = {
        ILI9488_PINS,                           /* boards/<BOARD>/board_pins.h — set by BOARD, not by us */
        .max_transfer_bytes = OUT * OUT * 2 + 16,
    };
    ESP_ERROR_CHECK(ili9488_init(&cfg));

    for (int i = 0; i < 16; i++) pal565[i] = ili9488_rgb565(PAL888[i][0], PAL888[i][1], PAL888[i][2]);

    ili9488_fill(ili9488_rgb565(0, 0, 0));   /* clear to black */

    /* build a 128x128 indexed test image: 16 vertical PICO-8 palette bars */
    static uint8_t idx[P8 * P8];
    for (int y = 0; y < P8; y++)
        for (int x = 0; x < P8; x++)
            idx[y * P8 + x] = (uint8_t)(x / (P8 / 16));    /* 8-px bars, colours 0..15 */

    /* palette-expand + 2x scale into a 256x256 RGB565 buffer */
    uint16_t *out = heap_caps_malloc(OUT * OUT * 2, MALLOC_CAP_DMA);
    if (!out) { ESP_LOGE(TAG, "no DMA buffer"); return; }
    for (int y = 0; y < OUT; y++) {
        const uint8_t *src = &idx[(y / SCALE) * P8];
        uint16_t *dst = &out[y * OUT];
        for (int x = 0; x < OUT; x++) dst[x] = pal565[src[x / SCALE]];
    }

    ili9488_blit(OX, OY, OUT, OUT, out);
    ESP_LOGI(TAG, "palette bars drawn at (%d,%d) %dx%d", OX, OY, OUT, OUT);

    /* The bus ceiling, derived from the pclk we ACTUALLY configured rather than pasted in as a
     * constant. On Bus_Parallel16 the panel takes COLMOD 0x55 (RGB565, 16bpp) and dlen_16bit puts
     * 16 bits on the wire per WR strobe -> exactly ONE bus cycle per pixel. (The ILI9488's
     * infamous RGB666/18-bit coercion is SPI-only: LovyanGFX applies it only when
     * busType()==bus_spi -- Panel_ILI948x.hpp:169-179. It does not apply to this board.)
     *
     * A hard-coded ceiling is what made the old log line incoherent: it kept saying
     * "~305 fps @16-bit/20MHz" long after the bus moved to 40 MHz, so a perfectly physical
     * 370 fps read as 21% past the theoretical maximum. Derive it, don't quote it. */
    const double ceiling_fps = (double)cfg.pclk_hz / (double)(OUT * OUT);

    ESP_LOGI(TAG, "bus ceiling %.1f fps  (%d-bit @ %.1f MHz, %dx%d RGB565, 1 cycle/px)",
             ceiling_fps, 16, cfg.pclk_hz / 1e6, OUT, OUT);

    /* Time N frames of `body`, reporting mean/min/max frame time and utilisation.
     * A fixed FRAME COUNT, not a fixed wall-clock window: it makes the sample auditable and
     * leaves no frames in flight at the edges. min/max matter because the gate asks for a
     * SUSTAINED rate, and a mean hides a stall. */
    #define MEASURE(label, N, body)                                                            \
        do {                                                                                   \
            for (int w = 0; w < 32; w++) { body; }          /* warm-up, untimed */             \
            int64_t _lo = INT64_MAX, _hi = 0;                                                  \
            int64_t _t0 = esp_timer_get_time(), _p = _t0;                                      \
            for (int f = 0; f < (N); f++) {                                                    \
                body;                                                                          \
                int64_t _n = esp_timer_get_time(), _d = _n - _p; _p = _n;                      \
                if (_d < _lo) _lo = _d;                                                        \
                if (_d > _hi) _hi = _d;                                                        \
            }                                                                                  \
            int64_t _el = _p - _t0;                                                            \
            double _fps = (N) * 1e6 / (double)_el;                                             \
            ESP_LOGI(TAG, "%-10s %4d frames in %7.3f ms -> %6.1f fps  "                        \
                          "(frame %.2f ms mean, %.2f min, %.2f max)  %.1f%% of ceiling",       \
                     label, (N), _el / 1000.0, _fps,                                           \
                     _el / 1000.0 / (N), _lo / 1000.0, _hi / 1000.0,                           \
                     100.0 * _fps / ceiling_fps);                                              \
            if (_fps > ceiling_fps)                                                            \
                ESP_LOGE(TAG, "%s: %.1f fps EXCEEDS the %.1f fps bus ceiling -- the clock "    \
                              "assumption is wrong, or the blit is not draining. The number "  \
                              "is a bug, not a record.", label, _fps, ceiling_fps);            \
        } while (0)

    /* Gate #1 asks for ">= 30 fps sustained for the 256x256 scaled full-refresh" -- and the plan's
     * A2 says "time the sustained FLUSH loop". Those are two different numbers, and reporting one
     * of them as "the" fps is how a display that drew nothing once passed at 288 fps. So report
     * both, always:
     *
     *   blit-only  -- push a pre-built 256x256 buffer. Answers "is the display path the
     *                 bottleneck?" (It is not.) This is A2's flush loop.
     *   end-to-end -- palette-expand + 2x scale + blit, EVERY frame. What a real cart pays, and
     *                 the number Gate #3/#4 inherit.
     *
     * The old loop rebuilt on 1 frame in 16 and so reported neither: ~15/16 of a blit rate,
     * labelled as the scaled full-refresh the gate actually asks about. */
    ESP_LOGI(TAG, "==== Gate #1 — PASS if >= 30 fps ====");

    while (1) {
        MEASURE("blit-only", 600, ili9488_blit(OX, OY, OUT, OUT, out));

        /* Yield between phases so IDLE0 can feed the task watchdog. The old loop never yielded,
         * so the TWDT fired at t~5.78s -- INSIDE the 5s measurement window -- and its backtrace
         * went out over a 115200 UART, depressing the very number being measured. Each phase here
         * is well under the 5s TWDT timeout, and this sits outside every timed region. */
        vTaskDelay(1);

        /* The palette advances every frame, so the panel is VISIBLY ANIMATING for the whole timed
         * window. That is deliberate and it is the point: a still of a static image is exactly
         * what 288 fps of DMA into unconnected pins would also have produced. A bench-camera frame
         * grabbed while this runs, showing content that changes between grabs, is the evidence
         * that turns a throughput counter into a display measurement. */
        MEASURE("end-to-end", 300, ({
            uint16_t p0 = pal565[1];
            for (int i = 1; i < 15; i++) pal565[i] = pal565[i + 1];
            pal565[15] = p0;
            for (int y = 0; y < OUT; y++) {
                const uint8_t *src = &idx[(y / SCALE) * P8];
                uint16_t *dst = &out[y * OUT];
                for (int x = 0; x < OUT; x++) dst[x] = pal565[src[x / SCALE]];
            }
            ili9488_blit(OX, OY, OUT, OUT, out);
        }));

        vTaskDelay(1);
    }
}
