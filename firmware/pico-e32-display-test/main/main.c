/* Phase-0 Track A — ILI9488 scaled PICO-8 blit + FPS (Gate #1).
 *
 * Board: Makerfabs "ESP32-S3 Parallel TFT with Touch (ILI9488)", 320x480,
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
 * Measured 2026-07-14: 288 fps (bus-level; visual colour/orientation still needs
 * the bench camera). If colours/orientation are wrong, adjust MADCTL / SWAP below. */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ili9488.h"

static const char *TAG = "trackA";

/* ---- Makerfabs ILI9488 parallel board pin map (vendor LovyanGFX config) ---- */
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
        .pin_wr = 18, .pin_dc = 17, .pin_cs = 46, .pin_bl = 45,   /* RD (48) unused by the write path */
        .data_pins = {47,21,14,13,12,11,10,9, 3,8,16,15,7,6,5,4},
        .pclk_hz = 20 * 1000 * 1000,                              /* vendor runs up to 20 MHz */
        .h_res = LCD_H_RES, .v_res = LCD_V_RES,
        .madctl = 0x48,                                           /* MX + BGR */
        .swap_color_bytes = true,                                 /* flip if colours look wrong */
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

    const int SECONDS = 5;
    int frames = 0;
    int64_t t0 = esp_timer_get_time(), t = t0;
    while (t - t0 < SECONDS * 1000000LL) {
        /* rotate the palette each frame so motion is visible while measuring */
        if ((frames & 0x0F) == 0) {
            uint16_t p0 = pal565[1];
            for (int i = 1; i < 15; i++) pal565[i] = pal565[i + 1];
            pal565[15] = p0;
            for (int y = 0; y < OUT; y++) {
                const uint8_t *src = &idx[(y / SCALE) * P8];
                uint16_t *dst = &out[y * OUT];
                for (int x = 0; x < OUT; x++) dst[x] = pal565[src[x / SCALE]];
            }
        }
        ili9488_blit(OX, OY, OUT, OUT, out);
        frames++;
        t = esp_timer_get_time();
    }
    double fps = frames / ((t - t0) / 1e6);
    ESP_LOGI(TAG, "==== Gate #1: %d frames in %.2fs = %.1f fps (256x256 blit) ====",
             frames, (t - t0) / 1e6, fps);
    ESP_LOGI(TAG, "PASS if >= 30 fps.  (bus ceiling ~305 fps @16-bit/20MHz)");

    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
