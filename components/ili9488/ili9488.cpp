/* ILI9488 over LovyanGFX Bus_Parallel16 — see ili9488.h.
 *
 * WHY LOVYANGFX AND NOT esp_lcd: on this board (Makerfabs 3.5" ILI9488, **first revision**)
 * the esp_lcd i80 path does not drive the panel at all. Three variants were tried on real
 * hardware -- but every one of those attempts used the WRONG PIN MAP (the newer board's
 * WR=18/DC=17/CS=46). Retried fairly on the correct rev-1 pins (2026-07-16), esp_lcd i80
 * renders geometry correctly but gets COLOUR wrong: red and yellow come out blue/green, with
 * only two distinguishable hues instead of four. A missing primary is disqualifying for a
 * PICO-8 console, so LovyanGFX stays. See docs/worklog/2026-07-16-panel-rev1-pinmap.md.
 *
 * The config below comes from the REV-1 source of record:
 *   references/makerfabs-parallel-tft-lvgl-lgfx/main/LGFX_MakerFabs_Parallel_S3.hpp
 *     - Bus_Parallel16 + Panel_ILI9488, WR=35 RS=36 CS=37 RD=48, freq_write 40 MHz,
 *     - pin_rst = -1 (RST is tied to the board reset),
 *     - dlen_16bit = true (this panel takes data in 16-bit units),
 *     - invert = false, rgb_order = false.
 *
 * DO NOT cite Makerfabs' own firmware/SD16_3.5/SD16_3.5.ino for these pins: that sketch was
 * updated in place for their NEWER board and now says WR=18/RS=17 -- the map that leaves this
 * panel blank. Same repo, same path, different revision, nothing marking the change. That is
 * exactly how two days were lost; see docs/worklog/2026-07-16-panel-rev1-pinmap.md.
 *
 * This is also what the plan of record asked for: docs/pico-e32-development-plan.md A1 --
 * "start from atanisoft/esp_lcd_ili9488 **or a LovyanGFX Bus_Parallel16 config**".
 */
#include "ili9488.h"
#include <string.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ili9488";

class PanelLGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9488  _panel;
    lgfx::Bus_Parallel16 _bus;
public:
    explicit PanelLGFX(const ili9488_config_t *c) {
        {
            auto b = _bus.config();
            b.freq_write = c->pclk_hz;
            b.pin_wr = c->pin_wr;
            b.pin_rd = c->pin_rd;
            b.pin_rs = c->pin_dc;            /* RS == D/C */
            b.pin_d0  = c->data_pins[0];  b.pin_d1  = c->data_pins[1];
            b.pin_d2  = c->data_pins[2];  b.pin_d3  = c->data_pins[3];
            b.pin_d4  = c->data_pins[4];  b.pin_d5  = c->data_pins[5];
            b.pin_d6  = c->data_pins[6];  b.pin_d7  = c->data_pins[7];
            b.pin_d8  = c->data_pins[8];  b.pin_d9  = c->data_pins[9];
            b.pin_d10 = c->data_pins[10]; b.pin_d11 = c->data_pins[11];
            b.pin_d12 = c->data_pins[12]; b.pin_d13 = c->data_pins[13];
            b.pin_d14 = c->data_pins[14]; b.pin_d15 = c->data_pins[15];
            _bus.config(b);
            _panel.setBus(&_bus);
        }
        {
            auto p = _panel.config();
            p.pin_cs   = c->pin_cs;          /* rev 1: a real GPIO (37), NOT tied low */
            p.pin_rst  = -1;
            p.pin_busy = -1;
            p.memory_width  = c->h_res;  p.memory_height = c->v_res;
            p.panel_width   = c->h_res;  p.panel_height  = c->v_res;
            p.offset_x = 0; p.offset_y = 0; p.offset_rotation = 0;
            p.dummy_read_pixel = 8; p.dummy_read_bits = 1;
            p.readable    = true;
            p.invert      = false;
            p.rgb_order   = false;
            p.dlen_16bit  = true;            /* this panel takes 16-bit data units */
            p.bus_shared  = true;            /* the microSD shares this bus */
            _panel.config(p);
        }
        setPanel(&_panel);
    }
};

static PanelLGFX      *s_lcd;
static ili9488_config_t s_cfg;

static void backlight_on(int pin) {
    if (pin < 0) return;
    ledc_timer_config_t t = { .speed_mode=LEDC_LOW_SPEED_MODE, .duty_resolution=LEDC_TIMER_8_BIT,
                              .timer_num=LEDC_TIMER_0, .freq_hz=5000, .clk_cfg=LEDC_AUTO_CLK };
    ledc_timer_config(&t);
    ledc_channel_config_t c = { .gpio_num=pin, .speed_mode=LEDC_LOW_SPEED_MODE, .channel=LEDC_CHANNEL_0,
                                .timer_sel=LEDC_TIMER_0, .duty=255, .hpoint=0 };
    ledc_channel_config(&c);
}

extern "C" esp_err_t ili9488_init(const ili9488_config_t *cfg) {
    s_cfg = *cfg;
    s_lcd = new PanelLGFX(&s_cfg);
    if (!s_lcd) return ESP_ERR_NO_MEM;
    if (!s_lcd->init()) { ESP_LOGE(TAG, "LovyanGFX init failed"); return ESP_FAIL; }
    backlight_on(cfg->pin_bl);
    ESP_LOGI(TAG, "ILI9488 up via LovyanGFX (%dx%d, %d Hz, cs=%d, dlen_16bit)",
             cfg->h_res, cfg->v_res, cfg->pclk_hz, cfg->pin_cs);
    return ESP_OK;
}

extern "C" void ili9488_blit(int x, int y, int w, int h, const uint16_t *src) {
    /* The apps hand us RGB565 already byte-swapped when swap_color_bytes is set, which is
     * LovyanGFX's swap565_t; otherwise it is native rgb565_t. Pick the matching type so no
     * extra conversion pass runs. pushImage blocks, so `src` is reusable on return. */
    if (s_cfg.swap_color_bytes) s_lcd->pushImage(x, y, w, h, (const lgfx::swap565_t *)src);
    else                        s_lcd->pushImage(x, y, w, h, (const lgfx::rgb565_t  *)src);
}

extern "C" void ili9488_fill(uint16_t color) {
    static uint16_t line[480];
    int w = s_cfg.h_res;
    for (int i = 0; i < w && i < (int)(sizeof(line)/sizeof(line[0])); i++) line[i] = color;
    for (int yy = 0; yy < s_cfg.v_res; yy++) ili9488_blit(0, yy, w, 1, line);
}

extern "C" uint16_t ili9488_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    if (s_cfg.swap_color_bytes) v = (uint16_t)((v >> 8) | (v << 8));
    return v;
}

/* Self-test: drive the panel with LovyanGFX's own high-level API only -- no palette, no
 * byte-swap, no pushImage, no framebuffer. If THIS shows nothing, the fault is below our
 * wrapper (library/bus/panel); if it shows colours, the fault is in our blit path. */
extern "C" void ili9488_selftest(void) {
    if (!s_lcd) { ESP_LOGE(TAG, "selftest: not initialised"); return; }
    const uint32_t cols[3] = { 0xFF0000u, 0x00FF00u, 0x0000FFu };   /* RGB888 red/green/blue */
    const char *names[3] = { "RED", "GREEN", "BLUE" };
    for (int i = 0; i < 3; i++) {
        s_lcd->fillScreen(s_lcd->color888((cols[i] >> 16) & 0xFF, (cols[i] >> 8) & 0xFF, cols[i] & 0xFF));
        ESP_LOGW(TAG, "selftest: fillScreen(%s)", names[i]);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
