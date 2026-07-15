/* ILI9488 over esp_lcd i80 — see ili9488.h. Extracted from the Phase-0
 * display-test harness (Gate #1); driver logic unchanged. */
#include <string.h>
#include "ili9488.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_lcd_panel_io.h"
#include "driver/ledc.h"
#include "esp_check.h"

static const char *TAG = "ili9488";

static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t         s_done;   /* given when a 0x2C colour DMA completes */
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

/* Minimal-but-complete ILI9488 init. Gamma/power are the common vendor set; the
 * essentials are sleep-out, 16-bit pixel format, MADCTL (from cfg) and display-on. */
static void panel_init(void) {
    wr(0x01, NULL, 0); vTaskDelay(pdMS_TO_TICKS(120));                 /* SW reset */
    wr(0x11, NULL, 0); vTaskDelay(pdMS_TO_TICKS(120));                 /* sleep out */
    wr(0xE0, (uint8_t[]){0x00,0x03,0x09,0x08,0x16,0x0A,0x3F,0x78,0x4C,0x09,0x0A,0x08,0x16,0x1A,0x0F},15);
    wr(0xE1, (uint8_t[]){0x00,0x16,0x19,0x03,0x0F,0x05,0x32,0x45,0x46,0x04,0x0E,0x0D,0x35,0x37,0x0F},15);
    wr(0xC0, (uint8_t[]){0x17,0x15}, 2);                              /* power control 1 */
    wr(0xC1, (uint8_t[]){0x41}, 1);                                   /* power control 2 */
    wr(0xC5, (uint8_t[]){0x00,0x12,0x80}, 3);                         /* VCOM */
    wr(0x36, (uint8_t[]){s_cfg.madctl}, 1);                           /* MADCTL: orientation + RGB/BGR */
    wr(0x3A, (uint8_t[]){0x55}, 1);                                   /* 16 bit/px (RGB565) */
    wr(0xB0, (uint8_t[]){0x00}, 1);
    wr(0xB1, (uint8_t[]){0xA0}, 1);                                   /* frame rate */
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
    return ESP_OK;
}

void ili9488_blit(int x, int y, int w, int h, const uint16_t *src) {
    set_window(x, y, x + w - 1, y + h - 1);
    esp_lcd_panel_io_tx_color(s_io, 0x2C, src, (size_t)w * h * 2);
    xSemaphoreTake(s_done, portMAX_DELAY);
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
