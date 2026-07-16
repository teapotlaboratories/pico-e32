/* ILI9488 over the ESP32-S3 esp_lcd i80 (16-bit Intel-8080) bus — the alternate backend.
 *
 * WHY THIS EXISTS: LovyanGFX (ili9488.cpp) is a 132 MB submodule for one panel driver. esp_lcd i80
 * is ESP-IDF-native and free. Both are sanctioned by the plan (§A1). This file lets the two be
 * built and compared without deleting either; pick with -D ILI9488_BACKEND=esp_lcd (see the
 * component CMakeLists and the repo Makefile). Same public API (ili9488.h), so apps are unchanged.
 *
 * Based on the hand-rolled esp_lcd driver that shipped before the LovyanGFX switch
 * (components/ili9488/ili9488.c @ git e0a21cc); driver logic mostly unchanged. Two deliberate
 * differences from that original, both this session's findings:
 *
 *   1. ORIENTATION. That driver wrote a raw cfg.madctl byte. .madctl is gone (it was dead config
 *      that named MX|BGR while nothing applied it). Orientation is now the board fact .mirror_y,
 *      and we compute the MADCTL from it: base BGR (0x08), plus MY|ML (0x90) when the glass is
 *      mounted mirrored -> 0x98, the exact value LovyanGFX proved upright this session. See
 *      docs/worklog/2026-07-16-yflip-and-gate1-fps.md and boards/makerfabs-ili9488-r1/board_pins.h.
 *
 *   2. COLOUR — UNRESOLVED. Speed is excellent (590 fps blit-only, 96.7% of the bus ceiling, vs
 *      LovyanGFX's 393 fps — this backend is REAL zero-copy DMA), but colour on this board is WRONG
 *      and is NOT a simple byte swap: on hardware every bright fill (red/green/blue alike) collapses
 *      to the SAME teal while brightness/structure survive, and toggling the app pre-swap changes
 *      NOTHING — which a byte-order bug could not do. Two hardware swap routes were tried and both
 *      fail: esp_lcd's flags.swap_color_bytes shreds a 16-bit frame, and crossing the data-pin array
 *      to mimic LovyanGFX's GPIO-matrix crossover (Bus_Parallel16.cpp:160-166) scrambles the 8-bit
 *      init COMMANDS that share the bus, so the panel never configures. The working theory is that
 *      this board's data-bus wiring needs LovyanGFX's exact crossover, which is entangled with how
 *      esp_lcd drives commands; confirming it needs a logic analyzer on the bus. Until then this
 *      backend is NOT the default and is kept for the speed comparison + future debugging.
 *      See docs/worklog/2026-07-16-esp-lcd-vs-lovyangfx.md. */
#include <string.h>
#include "ili9488.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_lcd_panel_io.h"
#include "driver/ledc.h"
#include "esp_check.h"

/* ILI9488 MADCTL bits (ESP-IDF names them in esp_lcd_panel_commands.h). */
#define MADCTL_MY  0x80   /* row address order   — 1: bottom-to-top (the vertical mirror) */
#define MADCTL_MX  0x40   /* column address order */
#define MADCTL_MV  0x20   /* row/column exchange (axis swap) */
#define MADCTL_ML  0x10   /* vertical refresh order — paired with MY to keep scanout consistent */
#define MADCTL_BGR 0x08   /* this panel is BGR */

static const char *TAG = "ili9488";

static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t         s_done;   /* given when a RAMWR (0x2C) colour DMA completes */
static ili9488_config_t          s_cfg;

static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t h,
                                    esp_lcd_panel_io_event_data_t *e, void *ctx) {
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &hp);
    return hp == pdTRUE;
}

static void wr(uint8_t cmd, const uint8_t *d, size_t n) {
    esp_lcd_panel_io_tx_param(s_io, cmd, d, n);
}

/* MADCTL for this board: BGR always; add the vertical mirror when the glass is mounted flipped.
 * 0x08 upright / 0x98 mirrored — 0x98 is what LovyanGFX (offset_rotation=4) was verified upright at. */
static uint8_t madctl_for(const ili9488_config_t *c) {
    return (uint8_t)(MADCTL_BGR | (c->mirror_y ? (MADCTL_MY | MADCTL_ML) : 0));
}

/* Minimal-but-complete ILI9488 init. Gamma/power are the common vendor set; the essentials are
 * sleep-out, 16-bit pixel format (COLMOD 0x55 = RGB565, one bus cycle/px on this parallel bus),
 * MADCTL (orientation + BGR) and display-on. */
static void panel_init(void) {
    wr(0x01, NULL, 0); vTaskDelay(pdMS_TO_TICKS(120));                 /* SW reset */
    wr(0x11, NULL, 0); vTaskDelay(pdMS_TO_TICKS(120));                 /* sleep out */
    wr(0xE0, (uint8_t[]){0x00,0x03,0x09,0x08,0x16,0x0A,0x3F,0x78,0x4C,0x09,0x0A,0x08,0x16,0x1A,0x0F},15);
    wr(0xE1, (uint8_t[]){0x00,0x16,0x19,0x03,0x0F,0x05,0x32,0x45,0x46,0x04,0x0E,0x0D,0x35,0x37,0x0F},15);
    wr(0xC0, (uint8_t[]){0x17,0x15}, 2);                              /* power control 1 */
    wr(0xC1, (uint8_t[]){0x41}, 1);                                   /* power control 2 */
    wr(0xC5, (uint8_t[]){0x00,0x12,0x80}, 3);                         /* VCOM */
    wr(0x36, (uint8_t[]){madctl_for(&s_cfg)}, 1);                     /* MADCTL: orientation + BGR */
    wr(0x3A, (uint8_t[]){0x55}, 1);                                   /* COLMOD: 16 bit/px (RGB565) */
    wr(0xB0, (uint8_t[]){0x00}, 1);
    wr(0xB1, (uint8_t[]){0xA0}, 1);                                   /* frame rate 60 Hz */
    wr(0xB4, (uint8_t[]){0x02}, 1);
    wr(0xE9, (uint8_t[]){0x00}, 1);
    wr(0xF7, (uint8_t[]){0xA9,0x51,0x2C,0x82}, 4);
    wr(0x29, NULL, 0); vTaskDelay(pdMS_TO_TICKS(50));                 /* display on */
}

static void set_window(int x0, int y0, int x1, int y1) {
    wr(0x2A, (uint8_t[]){(uint8_t)(x0>>8),(uint8_t)x0,(uint8_t)(x1>>8),(uint8_t)x1}, 4);
    wr(0x2B, (uint8_t[]){(uint8_t)(y0>>8),(uint8_t)y0,(uint8_t)(y1>>8),(uint8_t)y1}, 4);
}

static void backlight_on(int pin) {
    if (pin < 0) return;
    ledc_timer_config_t t = { .speed_mode=LEDC_LOW_SPEED_MODE, .duty_resolution=LEDC_TIMER_8_BIT,
                              .timer_num=LEDC_TIMER_0, .freq_hz=5000, .clk_cfg=LEDC_AUTO_CLK };
    ledc_timer_config(&t);
    ledc_channel_config_t c = { .gpio_num=pin, .speed_mode=LEDC_LOW_SPEED_MODE, .channel=LEDC_CHANNEL_0,
                                .timer_sel=LEDC_TIMER_0, .duty=255, .hpoint=0 };
    ledc_channel_config(&c);
}

esp_err_t ili9488_init(const ili9488_config_t *cfg) {
    s_cfg = *cfg;

    esp_lcd_i80_bus_handle_t bus = NULL;
    esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num = cfg->pin_dc, .wr_gpio_num = cfg->pin_wr, .clk_src = LCD_CLK_SRC_DEFAULT,
        .bus_width = 16, .max_transfer_bytes = cfg->max_transfer_bytes,
        .psram_trans_align = 64, .sram_trans_align = 4,
    };
    memcpy(bus_cfg.data_gpio_nums, cfg->data_pins, sizeof(bus_cfg.data_gpio_nums));
    ESP_RETURN_ON_ERROR(esp_lcd_new_i80_bus(&bus_cfg, &bus), TAG, "i80 bus");

    s_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_done, ESP_ERR_NO_MEM, TAG, "semaphore");

    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = cfg->pin_cs, .pclk_hz = cfg->pclk_hz, .trans_queue_depth = 10,
        .on_color_trans_done = on_color_done,
        .dc_levels = { .dc_idle_level=0, .dc_cmd_level=0, .dc_dummy_level=0, .dc_data_level=1 },
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i80(bus, &io_cfg, &s_io), TAG, "panel io");

    backlight_on(cfg->pin_bl);
    panel_init();
    ESP_LOGI(TAG, "ILI9488 up via esp_lcd i80 (%dx%d, %d Hz, cs=%d, madctl=0x%02x, app_preswap=%d)",
             cfg->h_res, cfg->v_res, cfg->pclk_hz, cfg->pin_cs, madctl_for(cfg), cfg->swap_color_bytes);
    return ESP_OK;
}

void ili9488_blit(int x, int y, int w, int h, const uint16_t *src) {
    set_window(x, y, x + w - 1, y + h - 1);
    esp_lcd_panel_io_tx_color(s_io, 0x2C, src, (size_t)w * h * 2);
    xSemaphoreTake(s_done, portMAX_DELAY);   /* blocks until the DMA transfer completes */
}

void ili9488_fill(uint16_t color) {
    static uint16_t line[480];               /* one row scratch, sized for the larger axis */
    int w = s_cfg.h_res;
    for (int i = 0; i < w && i < (int)(sizeof(line)/sizeof(line[0])); i++) line[i] = color;
    for (int yy = 0; yy < s_cfg.v_res; yy++) ili9488_blit(0, yy, w, 1, line);
}

uint16_t ili9488_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    if (s_cfg.swap_color_bytes) v = (uint16_t)((v >> 8) | (v << 8));
    return v;
}

/* Self-test: cycle the panel red/green/blue via the normal fill path (which exercises the byte-swap
 * and MADCTL exactly as a real frame would). If THIS shows the wrong colours, the byte-swap config
 * is wrong; if orientation is off, the MADCTL is. Diagnostic only. */
void ili9488_selftest(void) {
    const struct { uint8_t r, g, b; const char *name; } cols[3] = {
        {255,0,0,"RED"}, {0,255,0,"GREEN"}, {0,0,255,"BLUE"} };
    for (int i = 0; i < 3; i++) {
        ili9488_fill(ili9488_rgb565(cols[i].r, cols[i].g, cols[i].b));
        ESP_LOGW(TAG, "selftest: fill(%s)", cols[i].name);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
