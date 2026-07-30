/* Guition JC4880P443C-I-W (ESP32-P4) — board display implementation (GP-3).
 *
 * 480x800 ST7701S panel over MIPI-DSI: internal LDO (DPHY power) -> DSI bus -> DBI (command IO) ->
 * ST7701 vendor init -> DPI (video) panel with a PSRAM framebuffer. Blit/fill go through the DPI panel.
 *
 * ⚠️ CONFIG PROVENANCE — the pins, DSI params, panel timing, and ST7701 init sequence below are sourced
 * from ESPHome's board-specific model for this exact panel (esphome/esphome#12068, model "JC4880P443")
 * plus the ESPHomeDesigner #254 hardware profile. They are a strong, on-hardware-working candidate but are
 * NOT yet confirmed against THIS unit's schematic (vendor package JC4880P443C_I_W.zip), and this is EARLY
 * v1.x P4 silicon which may diverge from the v3.x parts those configs were tuned on. First light on the
 * bench camera is the oracle. See docs/hardware/pico-e32-guition-jc4880p443c-p4.md ("Candidate display
 * config") + docs/worklog/2026-07-29-gp3-p4-display-research.md.
 */
#include "board.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"   /* GT911 touch (v6 I2C master API) */

static const char *TAG = "board.p4";

/* --- Candidate config (schematic-UNVERIFIED — see file header). --- */
#define PIN_LCD_RST      GPIO_NUM_5    /* ESPHome mipi_dsi model reset_pin=5 (GPIO3=touch reset, not this) */
#define PIN_BL           GPIO_NUM_23   /* backlight enable/PWM; driven high = on (candidate) */

#define DSI_BUS_ID       0
#define DSI_LANES        2             /* 2-lane DSI */
#define DSI_LANE_MBPS    500           /* lane bit rate */
#define DPI_CLK_MHZ      34            /* DPI pixel clock */
#define DPHY_LDO_CHAN    3             /* P4-fixed: DPHY power on internal LDO channel 3 */
#define DPHY_LDO_MV      2500

/* Panel video timing (ESPHome JC4880P443). */
#define T_HSYNC 12
#define T_HBP   42
#define T_HFP   42
#define T_VSYNC 2
#define T_VBP   8
#define T_VFP   166

/* ST7701 vendor init sequence, verbatim from esphome guition.py model "JC4880P443".
 * Each row: command byte + its parameter bytes. 0xFF,0x77,0x01,0x00,0x00,0xNN selects command bank NN. */
typedef struct { uint8_t cmd; uint8_t len; uint8_t data[16]; } st7701_cmd_t;
static const st7701_cmd_t ST7701_INIT[] = {
    {0xFF, 5, {0x77,0x01,0x00,0x00,0x13}},
    {0xEF, 1, {0x08}},
    {0xFF, 5, {0x77,0x01,0x00,0x00,0x10}},
    {0xC0, 2, {0x63,0x00}},
    {0xC1, 2, {0x0D,0x02}},
    {0xC2, 2, {0x10,0x08}},
    {0xCC, 1, {0x10}},
    {0xB0, 16, {0x80,0x09,0x53,0x0C,0xD0,0x07,0x0C,0x09,0x09,0x28,0x06,0xD4,0x13,0x69,0x2B,0x71}},
    {0xB1, 16, {0x80,0x94,0x5A,0x10,0xD3,0x06,0x0A,0x08,0x08,0x25,0x03,0xD3,0x12,0x66,0x6A,0x0D}},
    {0xFF, 5, {0x77,0x01,0x00,0x00,0x11}},
    {0xB0, 1, {0x5D}},
    {0xB1, 1, {0x58}},
    {0xB2, 1, {0x87}},
    {0xB3, 1, {0x80}},
    {0xB5, 1, {0x4E}},
    {0xB7, 1, {0x85}},
    {0xB8, 1, {0x21}},
    {0xB9, 2, {0x10,0x1F}},
    {0xBB, 1, {0x03}},
    {0xBC, 1, {0x00}},
    {0xC1, 1, {0x78}},
    {0xC2, 1, {0x78}},
    {0xD0, 1, {0x88}},
    {0xE0, 3, {0x00,0x3A,0x02}},
    {0xE1, 11, {0x04,0xA0,0x00,0xA0,0x05,0xA0,0x00,0xA0,0x00,0x40,0x40}},
    {0xE2, 13, {0x30,0x00,0x40,0x40,0x32,0xA0,0x00,0xA0,0x00,0xA0,0x00,0xA0,0x00}},
    {0xE3, 4, {0x00,0x00,0x33,0x33}},
    {0xE4, 2, {0x44,0x44}},
    {0xE5, 16, {0x09,0x2E,0xA0,0xA0,0x0B,0x30,0xA0,0xA0,0x05,0x2A,0xA0,0xA0,0x07,0x2C,0xA0,0xA0}},
    {0xE6, 4, {0x00,0x00,0x33,0x33}},
    {0xE7, 2, {0x44,0x44}},
    {0xE8, 16, {0x08,0x2D,0xA0,0xA0,0x0A,0x2F,0xA0,0xA0,0x04,0x29,0xA0,0xA0,0x06,0x2B,0xA0,0xA0}},
    {0xEB, 7, {0x00,0x00,0x4E,0x4E,0x00,0x00,0x00}},
    {0xEC, 2, {0x08,0x01}},
    {0xED, 16, {0xB0,0x2B,0x98,0xA4,0x56,0x7F,0xFF,0xFF,0xFF,0xFF,0xF7,0x65,0x4A,0x89,0xB2,0x0B}},
    {0xEF, 6, {0x08,0x08,0x08,0x45,0x3F,0x54}},
    {0xFF, 5, {0x77,0x01,0x00,0x00,0x00}},
};

static esp_lcd_dsi_bus_handle_t s_bus = NULL;
static esp_lcd_panel_io_handle_t s_dbi = NULL;
static esp_lcd_panel_handle_t s_dpi = NULL;
static esp_ldo_channel_handle_t s_ldo = NULL;
static uint16_t *s_fb = NULL;                     /* the DPI scanout framebuffer (num_fbs=1, in PSRAM) */
static const size_t FB_BYTES = (size_t)BOARD_LCD_H_RES * BOARD_LCD_V_RES * 2;

static void gpio_out(gpio_num_t pin, int level) {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << pin;
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level(pin, level);
}

extern "C" esp_err_t board_lcd_init(void) {
    /* 1. Power the MIPI-DSI DPHY via the internal LDO (P4: channel 3 @ 2.5 V). */
    esp_ldo_channel_config_t ldo_cfg = {};
    ldo_cfg.chan_id = DPHY_LDO_CHAN;
    ldo_cfg.voltage_mv = DPHY_LDO_MV;
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &s_ldo), TAG, "LDO acquire (DPHY power)");

    /* 2. Hardware-reset the ST7701 (active-low pulse). */
    gpio_out(PIN_LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_LCD_RST, 0); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(120));

    /* 3. DSI bus (2 lanes @ 500 Mbps). */
    esp_lcd_dsi_bus_config_t bus_cfg = {};
    bus_cfg.bus_id = DSI_BUS_ID;
    bus_cfg.num_data_lanes = DSI_LANES;
    bus_cfg.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    bus_cfg.lane_bit_rate_mbps = DSI_LANE_MBPS;
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &s_bus), TAG, "new DSI bus");

    /* 4. DBI command IO (for the ST7701 init writes). */
    esp_lcd_dbi_io_config_t dbi_cfg = {};
    dbi_cfg.virtual_channel = 0;
    dbi_cfg.lcd_cmd_bits = 8;
    dbi_cfg.lcd_param_bits = 8;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_bus, &dbi_cfg, &s_dbi), TAG, "new DBI IO");

    /* 5. ST7701 vendor init sequence. */
    for (size_t i = 0; i < sizeof(ST7701_INIT) / sizeof(ST7701_INIT[0]); i++) {
        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_io_tx_param(s_dbi, ST7701_INIT[i].cmd, ST7701_INIT[i].data, ST7701_INIT[i].len),
            TAG, "ST7701 init cmd 0x%02X", ST7701_INIT[i].cmd);
    }
    /* Sleep-out + display-on (standard DCS). */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_dbi, 0x11, NULL, 0), TAG, "sleep out");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(s_dbi, 0x29, NULL, 0), TAG, "display on");
    vTaskDelay(pdMS_TO_TICKS(20));

    /* 6. DPI video panel + PSRAM framebuffer. */
    esp_lcd_dpi_panel_config_t dpi_cfg = {};
    dpi_cfg.virtual_channel = 0;
    dpi_cfg.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_cfg.dpi_clock_freq_mhz = DPI_CLK_MHZ;
    dpi_cfg.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi_cfg.out_color_format = LCD_COLOR_FMT_RGB565;
    dpi_cfg.num_fbs = 1;
    dpi_cfg.video_timing.h_size = BOARD_LCD_H_RES;
    dpi_cfg.video_timing.v_size = BOARD_LCD_V_RES;
    dpi_cfg.video_timing.hsync_pulse_width = T_HSYNC;
    dpi_cfg.video_timing.hsync_back_porch = T_HBP;
    dpi_cfg.video_timing.hsync_front_porch = T_HFP;
    dpi_cfg.video_timing.vsync_pulse_width = T_VSYNC;
    dpi_cfg.video_timing.vsync_back_porch = T_VBP;
    dpi_cfg.video_timing.vsync_front_porch = T_VFP;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dpi(s_bus, &dpi_cfg, &s_dpi), TAG, "new DPI panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_dpi), TAG, "DPI panel init");
    esp_lcd_dpi_panel_get_frame_buffer(s_dpi, 1, (void **)&s_fb);

    /* 7. Backlight on. */
    gpio_out(PIN_BL, 1);

    ESP_LOGI(TAG, "ST7701S/MIPI-DSI up: %dx%d, %d-lane @ %d Mbps, DPI %d MHz (config UNVERIFIED vs schematic)",
             BOARD_LCD_H_RES, BOARD_LCD_V_RES, DSI_LANES, DSI_LANE_MBPS, DPI_CLK_MHZ);
    return ESP_OK;
}

extern "C" void board_lcd_blit(int x, int y, int w, int h, const uint16_t *src) {
    if (!s_dpi || !src) return;
    esp_lcd_panel_draw_bitmap(s_dpi, x, y, x + w, y + h, src);
}

extern "C" void board_lcd_fill(uint16_t color) {
    if (!s_fb) return;
    size_t px = (size_t)BOARD_LCD_H_RES * BOARD_LCD_V_RES;
    for (size_t i = 0; i < px; i++) s_fb[i] = color;
    /* num_fbs=1: s_fb IS the scanout buffer (PSRAM, cached) — flush so the DPI DMA sees the new pixels. */
    esp_cache_msync(s_fb, FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

extern "C" uint16_t board_lcd_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    /* Standard little-endian RGB565; DPI in_color_format is RGB565 and the panel color order is RGB. */
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

extern "C" int board_lcd_width(void)  { return BOARD_LCD_H_RES; }
extern "C" int board_lcd_height(void) { return BOARD_LCD_V_RES; }

extern "C" const uint16_t *board_lcd_framebuffer(int *w, int *h) {
    if (w) *w = BOARD_LCD_H_RES;
    if (h) *h = BOARD_LCD_V_RES;
    return s_fb;   /* the DPI scan-out buffer (num_fbs=1): blit/fill composite straight into it */
}

/* ---- GT911 capacitive touch (I2C). Candidate pins from ESPHomeDesigner #254 (SDA7/SCL8, 400 kHz);
 * addr 0x5D is the GT911 default (0x14 if INT is strapped high at reset — probed as a fallback). GT911
 * uses 16-bit register addresses. Refs: GT911 programming guide. ---- */
#define TOUCH_SDA        GPIO_NUM_7
#define TOUCH_SCL        GPIO_NUM_8
#define TOUCH_ADDR_MAIN  0x5D
#define TOUCH_ADDR_ALT   0x14
#define GT_REG_PRODUCT_ID 0x8140   /* 4 bytes, ASCII "911\0" */
#define GT_REG_STATUS     0x814E   /* bit7 = buffer ready; bits[3:0] = touch count */
#define GT_REG_POINT1_X   0x8150   /* per-point stride 8: xlo,xhi,ylo,yhi,szlo,szhi,rsv,(next id) */

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_touch = NULL;

static esp_err_t gt_rd(uint16_t reg, uint8_t *buf, size_t len) {
    uint8_t a[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return i2c_master_transmit_receive(s_touch, a, 2, buf, len, pdMS_TO_TICKS(100));
}
static esp_err_t gt_wr8(uint16_t reg, uint8_t val) {
    uint8_t a[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val };
    return i2c_master_transmit(s_touch, a, 3, pdMS_TO_TICKS(100));
}

extern "C" esp_err_t board_touch_init(void) {
    i2c_master_bus_config_t bus = {};
    bus.i2c_port = I2C_NUM_0;
    bus.sda_io_num = TOUCH_SDA;
    bus.scl_io_num = TOUCH_SCL;
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_i2c_bus), TAG, "touch: I2C bus");

    /* Try the default address first, then the alternate. */
    const uint8_t addrs[] = { TOUCH_ADDR_MAIN, TOUCH_ADDR_ALT };
    for (uint8_t addr : addrs) {
        i2c_device_config_t dev = {};
        dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev.device_address = addr;
        dev.scl_speed_hz = 400000;
        if (i2c_master_bus_add_device(s_i2c_bus, &dev, &s_touch) != ESP_OK) continue;
        uint8_t id[4] = {0};
        if (gt_rd(GT_REG_PRODUCT_ID, id, 4) == ESP_OK &&
            (id[0] == '9' || id[0] == '8')) {   /* GT911="911", some report "9147"/others start 8/9 */
            ESP_LOGI(TAG, "GT911 touch up: id \"%c%c%c%c\" @ 0x%02X (SDA=%d SCL=%d)",
                     id[0], id[1], id[2], id[3], addr, TOUCH_SDA, TOUCH_SCL);
            return ESP_OK;
        }
        i2c_master_bus_rm_device(s_touch);
        s_touch = NULL;
    }
    ESP_LOGW(TAG, "GT911 not found at 0x%02X/0x%02X — touch disabled", TOUCH_ADDR_MAIN, TOUCH_ADDR_ALT);
    return ESP_ERR_NOT_FOUND;
}

extern "C" int board_touch_read(int *xs, int *ys, int max) {
    if (!s_touch || max <= 0) return 0;
    uint8_t status = 0;
    esp_err_t rr = gt_rd(GT_REG_STATUS, &status, 1);
#ifdef TOUCH_DEBUG
    static int64_t s_dbg_next = 0;
    int64_t nowd = esp_timer_get_time();
    if (rr != ESP_OK) {
        if (nowd >= s_dbg_next) { s_dbg_next = nowd + 500000; ESP_LOGW(TAG, "GT911 status read err: %s", esp_err_to_name(rr)); }
    } else if (status != 0 && nowd >= s_dbg_next) {
        s_dbg_next = nowd + 200000; ESP_LOGI(TAG, "GT911 status=0x%02X (ready=%d count=%d)", status, !!(status & 0x80), status & 0x0F);
    }
#endif
    if (rr != ESP_OK) return 0;
    if (!(status & 0x80)) return 0;              /* buffer not ready — no new frame */
    int n = status & 0x0F;
    if (n > 5) n = 5;
    int out = 0;
    if (n > 0) {
        uint8_t pts[5 * 8] = {0};
        if (gt_rd(GT_REG_POINT1_X, pts, (size_t)n * 8) == ESP_OK) {
            for (int i = 0; i < n && out < max; i++) {
                int rx = pts[i * 8 + 0] | (pts[i * 8 + 1] << 8);
                int ry = pts[i * 8 + 2] | (pts[i * 8 + 3] << 8);
                /* Passed through raw as display coords (panel is native-portrait upright). Touch<->display
                 * axis alignment is not yet HITL-confirmed (GP-5 open item). */
                xs[out] = rx;
                ys[out] = ry;
                out++;
            }
        }
    }
    gt_wr8(GT_REG_STATUS, 0);                    /* clear ready flag so the controller posts the next frame */
    return out;
}
