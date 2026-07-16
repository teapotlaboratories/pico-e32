/* Makerfabs ESP32-S3 Parallel TFT with Touch (ILI9488) 3.5" — FIRST REVISION — display driver.
 *
 * The board's display bring-up: a LovyanGFX Bus_Parallel16 + Panel_ILI9488 with THIS board's pins,
 * orientation and byte order baked in. It implements the board-agnostic API in board.h so apps draw
 * without naming a vendor, revision, pin, or driver library. (Until 2026-07-16 this lived in a
 * reusable components/ili9488 wrapper with a config struct; with only one board and one backend
 * that indirection earned its keep no more, so the board owns its own display.)
 *
 * ┌──────────────────────────────────────────────────────────────────────────────────────┐
 * │ ⚠ THIS BOARD HAS TWO PIN MAPS. WHICH ONE DEPENDS ON THE PSRAM PART.                  │
 * │                                                                                       │
 * │   rev 1 (THIS UNIT)  N16R2 · 2 MB QUAD PSRAM  → 35/36/37 free  → LCD WR=35 DC=36 CS=37│
 * │   current rev        N16R8 · 8 MB OCTAL PSRAM → 35/36/37 taken → LCD WR=18 DC=17 CS=46│
 * │                                                                                       │
 * │ ESP32-S3 octal PSRAM occupies GPIO 35/36/37, so Makerfabs relocated the LCD when they  │
 * │ moved to the N16R8 part. Both maps are published by the vendor, for different boards,  │
 * │ with nothing marking which is which — their firmware/SD16_3.5/SD16_3.5.ino was even    │
 * │ edited IN PLACE across the revision. The WRONG map leaves the panel backlit-white while │
 * │ DMA completes, checksums match, and every diagnostic reports success. It cost two days. │
 * │ Check the revision, not just the vendor. See docs/worklog/2026-07-16-panel-rev1-pinmap.md.│
 * └──────────────────────────────────────────────────────────────────────────────────────┘
 *
 * Source of record (pinned): references/makerfabs-parallel-tft-lvgl-lgfx @ 6d4b014,
 *   main/LGFX_MakerFabs_Parallel_S3.hpp — Bus_Parallel16 + Panel_ILI9488, WR 35 RS 36 CS 37 RD 48,
 *   40 MHz, pin_rst=-1, dlen_16bit=true, invert=false, rgb_order=false.
 * DO NOT cite Makerfabs' own firmware/SD16_3.5/SD16_3.5.ino for these pins: it was updated in place
 * for their NEWER board (WR=18/RS=17), the map that leaves this panel blank.
 *
 * WHY LOVYANGFX, NOT esp_lcd: LovyanGFX renders this panel correctly at 393 fps. An esp_lcd i80
 * backend was built and bench-tested (2026-07-16): faster (590 fps, zero-copy DMA) but its colour is
 * broken on this board in a way byte-swapping does not fix, likely because it can't reproduce
 * LovyanGFX's data-pin crossover without scrambling the shared init commands. Removed for now; the
 * findings live in docs/worklog/2026-07-16-esp-lcd-vs-lovyangfx.md.
 */
#include "board.h"
#include <string.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board-lcd";

/* ---- THIS BOARD'S WIRING (rev 1) — the board fact, in exactly one place ---- */
#define PIN_WR 35                 /* WR strobe */
#define PIN_DC 36                 /* data/command (RS) */
#define PIN_CS 37                 /* rev 1: a real GPIO, NOT tied low */
#define PIN_BL 45                 /* backlight (PWM via LEDC) */
#define PIN_RD 48                 /* RD strobe — driven HIGH on this 8080 bus; a pin, not a strobe we generate */
static const int8_t DATA_PINS[16] = { 47, 21, 14, 13, 12, 11, 10, 9,
                                       3,  8, 16, 15,  7,  6,  5, 4 };   /* D0..D15 — identical on both revisions */
/* PCLK is BOARD_LCD_PCLK_HZ (board.h) — one definition, shared with any benchmark that needs it. */

/* ORIENTATION: the glass is mounted upside-down on this board — at the controller's native scan
 * direction GRAM row 0 lands at the BOTTOM, so everything renders vertically mirrored. A board fact
 * (measured via the L-pattern, docs/worklog/2026-07-16-yflip-and-gate1-fps.md), not a driver fact.
 * The vendor's own rev-1 config does NOT set it because their demo only ever ran setRotation(1)
 * (landscape) — rotation 0 was untested upstream, so "matches the vendor" was never evidence.
 * Applied as LovyanGFX offset_rotation=4 (MADCTL MY|ML, 0x98 with BGR): the only value in 0-7 that
 * inverts Y while leaving MX and the axis-swap MV clear, so 320x480 and the centred blit are safe. */
static const bool MIRROR_Y = true;

/* BYTE ORDER: this panel takes byte-SWAPPED RGB565 on LovyanGFX's swap565_t fast path. board_lcd_rgb565
 * pre-swaps and board_lcd_blit hands over swap565_t, so no per-pixel conversion pass runs. */
static const bool SWAP_COLOR_BYTES = true;

class PanelLGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9488  _panel;
    lgfx::Bus_Parallel16 _bus;
public:
    PanelLGFX() {
        {
            auto b = _bus.config();
            b.freq_write = BOARD_LCD_PCLK_HZ;
            b.pin_wr = PIN_WR;
            b.pin_rd = PIN_RD;
            b.pin_rs = PIN_DC;               /* RS == D/C */
            b.pin_d0  = DATA_PINS[0];  b.pin_d1  = DATA_PINS[1];
            b.pin_d2  = DATA_PINS[2];  b.pin_d3  = DATA_PINS[3];
            b.pin_d4  = DATA_PINS[4];  b.pin_d5  = DATA_PINS[5];
            b.pin_d6  = DATA_PINS[6];  b.pin_d7  = DATA_PINS[7];
            b.pin_d8  = DATA_PINS[8];  b.pin_d9  = DATA_PINS[9];
            b.pin_d10 = DATA_PINS[10]; b.pin_d11 = DATA_PINS[11];
            b.pin_d12 = DATA_PINS[12]; b.pin_d13 = DATA_PINS[13];
            b.pin_d14 = DATA_PINS[14]; b.pin_d15 = DATA_PINS[15];
            _bus.config(b);
            _panel.setBus(&_bus);
        }
        {
            auto p = _panel.config();
            p.pin_cs   = PIN_CS;
            p.pin_rst  = -1;                 /* RST tied to the board reset */
            p.pin_busy = -1;
            p.memory_width  = BOARD_LCD_H_RES;  p.memory_height = BOARD_LCD_V_RES;
            p.panel_width   = BOARD_LCD_H_RES;  p.panel_height  = BOARD_LCD_V_RES;
            p.offset_x = 0; p.offset_y = 0;
            p.offset_rotation = MIRROR_Y ? 4 : 0;   /* see MIRROR_Y above */
            p.dummy_read_pixel = 8; p.dummy_read_bits = 1;
            p.readable    = true;
            p.invert      = false;
            p.rgb_order   = false;           /* BGR */
            p.dlen_16bit  = true;            /* this panel takes 16-bit data units */
            p.bus_shared  = true;
            _panel.config(p);
        }
        setPanel(&_panel);
    }
};

static PanelLGFX *s_lcd;

static void backlight_on(int pin) {
    if (pin < 0) return;
    ledc_timer_config_t t = { .speed_mode=LEDC_LOW_SPEED_MODE, .duty_resolution=LEDC_TIMER_8_BIT,
                              .timer_num=LEDC_TIMER_0, .freq_hz=5000, .clk_cfg=LEDC_AUTO_CLK };
    ledc_timer_config(&t);
    ledc_channel_config_t c = { .gpio_num=pin, .speed_mode=LEDC_LOW_SPEED_MODE, .channel=LEDC_CHANNEL_0,
                                .timer_sel=LEDC_TIMER_0, .duty=255, .hpoint=0 };
    ledc_channel_config(&c);
}

extern "C" esp_err_t board_lcd_init(void) {
    s_lcd = new PanelLGFX();
    if (!s_lcd) return ESP_ERR_NO_MEM;
    if (!s_lcd->init()) { ESP_LOGE(TAG, "LovyanGFX init failed"); return ESP_FAIL; }
    backlight_on(PIN_BL);
    ESP_LOGI(TAG, "LCD up via LovyanGFX (%dx%d, %d Hz, cs=%d, mirror_y=%d, dlen_16bit)",
             BOARD_LCD_H_RES, BOARD_LCD_V_RES, BOARD_LCD_PCLK_HZ, PIN_CS, MIRROR_Y);
    return ESP_OK;
}

extern "C" void board_lcd_blit(int x, int y, int w, int h, const uint16_t *src) {
    /* One transaction per blit, so the unnested endWrite() drains it — the blit blocks until the
     * transfer completes and `src` is reusable. Do NOT wrap several blits in an outer
     * startWrite()/endWrite(): that moves the drain outward and turns any fps loop into a
     * queueing-rate measurement. See docs/worklog/2026-07-16-yflip-and-gate1-fps.md. */
    if (SWAP_COLOR_BYTES) s_lcd->pushImage(x, y, w, h, (const lgfx::swap565_t *)src);
    else                  s_lcd->pushImage(x, y, w, h, (const lgfx::rgb565_t  *)src);
}

extern "C" void board_lcd_fill(uint16_t color) {
    static uint16_t line[BOARD_LCD_V_RES];
    for (int i = 0; i < BOARD_LCD_H_RES; i++) line[i] = color;
    for (int yy = 0; yy < BOARD_LCD_V_RES; yy++) board_lcd_blit(0, yy, BOARD_LCD_H_RES, 1, line);
}

extern "C" uint16_t board_lcd_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    if (SWAP_COLOR_BYTES) v = (uint16_t)((v >> 8) | (v << 8));
    return v;
}

extern "C" void board_lcd_selftest(void) {
    if (!s_lcd) { ESP_LOGE(TAG, "selftest: not initialised"); return; }
    const uint32_t cols[3] = { 0xFF0000u, 0x00FF00u, 0x0000FFu };
    const char *names[3] = { "RED", "GREEN", "BLUE" };
    for (int i = 0; i < 3; i++) {
        s_lcd->fillScreen(s_lcd->color888((cols[i] >> 16) & 0xFF, (cols[i] >> 8) & 0xFF, cols[i] & 0xFF));
        ESP_LOGW(TAG, "selftest: fillScreen(%s)", names[i]);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
