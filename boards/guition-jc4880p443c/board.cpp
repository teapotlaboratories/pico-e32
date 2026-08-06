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
#include "input.h"                /* INPUT_* button bits, for board_touch_hittest */
#include "pico8_font.h"           /* {char c; const char *rows[5];} PICO8_FONT_GLYPHS[] — deck labels */
#include <string.h>
#include <stdlib.h>               /* malloc/free (deck raster buffers) */
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
#include "driver/i2c_master.h"   /* GT911 touch + ES8311 codec share this I2C bus */
#include "driver/i2s_std.h"      /* ES8311 audio: I2S standard TX */
#include "esp_codec_dev.h"       /* esp_codec_dev (vendored managed component) */
#include "esp_codec_dev_defaults.h"   /* pulls in es8311_codec.h: es8311_codec_new + ES8311_CODEC_DEFAULT_ADDR */
#include "driver/sdmmc_host.h"   /* SDMMC host (slot 0, 4-bit) for the TF/microSD slot */
#include "sdmmc_cmd.h"           /* sdmmc_card_t */
#include "esp_vfs_fat.h"         /* esp_vfs_fat_sdmmc_mount / esp_vfs_fat_sdcard_unmount */
#include "sd_pwr_ctrl_by_on_chip_ldo.h"  /* the P4 powers the SD 3.3V rail from an on-chip LDO (VO4) */

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

/* The GT911 touch controller (0x5D/0x14) and the ES8311 codec (0x18) share SDA7/SCL8, so the I2C master
 * bus is a lazily-created shared resource: whichever of board_touch_init / board_audio_init runs first
 * creates it, the other reuses it. */
static esp_err_t ensure_i2c_bus(void) {
    if (s_i2c_bus) return ESP_OK;
    i2c_master_bus_config_t bus = {};
    bus.i2c_port = I2C_NUM_0;
    bus.sda_io_num = TOUCH_SDA;
    bus.scl_io_num = TOUCH_SCL;
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = true;
    return i2c_new_master_bus(&bus, &s_i2c_bus);
}

extern "C" esp_err_t board_touch_init(void) {
    if (s_touch) return ESP_OK;   /* idempotent: the carousel launcher inits touch before the game loop's
                                   * input backend does — a second add_device would double-register. */
    ESP_RETURN_ON_ERROR(ensure_i2c_bus(), TAG, "touch: I2C bus");

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

/* ===================== On-screen touch control deck — P4-owned layout + rendering =====================
 * Rasterised into the DPI framebuffer via board_lcd_blit + board_lcd_rgb565. That path is a framebuffer
 * memcpy, so odd-sized buffers are fine (unlike the S3's i80 DMA, which the S3 board draws around with
 * LovyanGFX vectors). Layout is derived from this panel's geometry; draw + hit-test share it. Matches the
 * approved mockup, docs/runtime/pico-e32-fake08-touch-ui.html. Moved here from the shared input layer so
 * the board owns its screen layout + rendering. */
namespace {

/* Deck layout, derived from panel geometry. */
static struct {
    int dpad_cx, dpad_cy, reach, dead, barH, barW;
    int o_cx, o_cy, x_cx, x_cy, btn_r;
    int menu_cx, menu_cy, menu_w, menu_h;
    int lbl;   /* label glyph scale */
} L;

static void layout_init(void) {
    int W = board_lcd_width();
    int H = board_lcd_height();
    /* Game height = 128 * integer scale, matching ESP32Host's pico_scale(W), so the deck sits exactly
     * below the game: P4 480 -> 3x -> 384. */
    int scale = W / 128; if (scale < 2) scale = 2; if (scale > 3) scale = 3;
    int game_h = 128 * scale;
    int deckH = H - game_h; if (deckH < 0) deckH = 0;
    L.dpad_cx = W * 23 / 80;          L.dpad_cy = game_h + deckH * 120 / 224;
    L.reach   = 74 * W / 320;         L.dead    = 16 * W / 320;
    L.barH    = 50 * W / 320;         L.barW    = 50 * W / 320;
    L.o_cx    = W * 53 / 80;          L.o_cy    = game_h + deckH * 158 / 224;
    L.x_cx    = W * 68 / 80;          L.x_cy    = game_h + deckH * 96 / 224;
    L.btn_r   = 31 * W / 320;
    L.menu_cx = W / 2;                L.menu_cy = game_h + deckH * 27 / 224;
    L.menu_w  = 58 * W / 320;         L.menu_h  = 22 * W / 320;
    L.lbl     = 2 * W / 320; if (L.lbl < 1) L.lbl = 1;
}

static inline int sq(int v) { return v * v; }

/* Linear colour interpolation -> board RGB565, for the gradients (matches the mockup's fills). */
static uint16_t lerp565(int r0, int g0, int b0, int r1, int g1, int b1, int num, int den) {
    if (den <= 0) den = 1;
    if (num < 0) num = 0;
    if (num > den) num = den;
    return board_lcd_rgb565((uint8_t)(r0 + (r1 - r0) * num / den),
                            (uint8_t)(g0 + (g1 - g0) * num / den),
                            (uint8_t)(b0 + (b1 - b0) * num / den));
}

/* Deck surface colour — a subtle dark blue-grey (the mockup's deck tone), NOT pure black; must match
 * ESP32Host's one-time panel fill. */
static uint16_t deck_bg(void) { return board_lcd_rgb565(0x0f, 0x14, 0x1d); }

/* Rounded-rect membership: is local pixel (rx,ry) inside a w×h rect with corner radius cr? */
static int in_rrect(int rx, int ry, int w, int h, int cr) {
    if (cr <= 0) return 1;
    int ccx = rx < cr ? cr : (rx >= w - cr ? w - 1 - cr : -1);
    int ccy = ry < cr ? cr : (ry >= h - cr ? h - 1 - cr : -1);
    if (ccx < 0 || ccy < 0) return 1;              /* not in a corner region -> inside */
    int dx = rx - ccx, dy = ry - ccy;
    return dx * dx + dy * dy <= cr * cr;
}

/* Small direction chevron into buffer b (W×H). dir 0/1/2/3 = up/down/left/right; apex at (ax,ay). */
static void chevron(uint16_t *b, int W, int H, int dir, int ax, int ay, int hw, int hh, uint16_t col) {
    if (hh <= 0) return;
    for (int r = 0; r <= hh; r++) {
        int half = hw * r / hh;
        for (int e = -half; e <= half; e++) {
            int px, py;
            if (dir == 0)      { px = ax + e; py = ay + r; }   /* up: apex top, widen down */
            else if (dir == 1) { px = ax + e; py = ay - r; }   /* down: apex bottom, widen up */
            else if (dir == 2) { px = ax + r; py = ay + e; }   /* left: apex left, widen right */
            else               { px = ax - r; py = ay + e; }   /* right: apex right, widen left */
            if (px >= 0 && px < W && py >= 0 && py < H) b[py * W + px] = col;
        }
    }
}

/* Rounded-rect OUTLINE (border only; interior = bg) — the MENU pill is fill:none in the mockup. */
static void rrect_outline(int x, int y, int w, int h, int cr, int t, uint16_t col, uint16_t bg) {
    if (w <= 0 || h <= 0) return;
    uint16_t *b = (uint16_t *)malloc((size_t)w * h * 2); if (!b) return;
    for (int ry = 0; ry < h; ry++)
        for (int rx = 0; rx < w; rx++) {
            int inside = in_rrect(rx, ry, w, h, cr);
            int inner  = (rx >= t && ry >= t && rx < w - t && ry < h - t) &&
                         in_rrect(rx - t, ry - t, w - 2 * t, h - 2 * t, cr - t);
            b[ry * w + rx] = (inside && !inner) ? col : bg;
        }
    board_lcd_blit(x, y, w, h, b); free(b);
}

/* O/X button: a SPHERICAL radial shade + a thin coloured ring + a clean vector glyph on top. */
static void disc_sphere(int cx, int cy, int r, uint16_t ring, uint16_t bg, char label, uint16_t lbl) {
    int s = 2 * r + 1, rr = r * r, hy = r * 35 / 100;   /* highlight sits above centre */
    uint16_t *b = (uint16_t *)malloc((size_t)s * s * 2); if (!b) return;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            uint16_t c = bg;
            if (d2 <= rr) {
                int hdy = dy + hy;                        /* distance from the highlight point (0,-hy) */
                int t = (dx * dx + hdy * hdy) * 95 / rr;  /* squared falloff reads as a soft sphere */
                c = lerp565(0x2f, 0x37, 0x43, 0x16, 0x1b, 0x23, t, 95);
                if (d2 >= (r - 2) * (r - 2)) c = ring;    /* thin coloured ring */
            }
            b[(dy + r) * s + (dx + r)] = c;
        }
    int th = r / 9; if (th < 1) th = 1;                   /* glyph stroke half-thickness */
    if (label == 'O') {
        int rg = r * 42 / 100;                            /* glyph ring radius */
        for (int dy = -rg - th; dy <= rg + th; dy++)
            for (int dx = -rg - th; dx <= rg + th; dx++) {
                int d2 = dx * dx + dy * dy;
                if (d2 <= (rg + th) * (rg + th) && d2 >= (rg - th) * (rg - th))
                    b[(dy + r) * s + (dx + r)] = lbl;
            }
    } else if (label == 'X') {
        int len = r * 40 / 100;
        for (int u = -len; u <= len; u++)
            for (int w2 = -th; w2 <= th; w2++) {
                int y1 = r + u + w2, x1 = r + u;          /* '\' diagonal */
                int y2 = r - u + w2, x2 = r + u;          /* '/' diagonal */
                if (x1 >= 0 && x1 < s && y1 >= 0 && y1 < s) b[y1 * s + x1] = lbl;
                if (x2 >= 0 && x2 < s && y2 >= 0 && y2 < s) b[y2 * s + x2] = lbl;
            }
    }
    board_lcd_blit(cx - r, cy - r, s, s, b); free(b);
}

/* D-pad: two vertically-graded rounded bars forming a cross, four soft-blue chevrons, a dark hub. */
static void draw_dpad(void) {
    int cx = L.dpad_cx, cy = L.dpad_cy, reach = L.reach;
    int top = cy - reach, bot = cy + reach;             /* gradient spans the whole cross */
    uint16_t chev = board_lcd_rgb565(0x7f, 0xa8, 0xe6);
    uint16_t bg = deck_bg();
    int chh = L.barH / 5; if (chh < 3) chh = 3;         /* chevron height (apex -> base) */
    int chw = L.barH * 4 / 25; if (chw < 2) chw = 2;    /* chevron half-base width */
    int inset = L.barH * 8 / 25;                        /* chevron apex inset from each arm tip */

    /* horizontal bar (rounded) + left/right chevrons */
    { int x0 = cx - reach, y0 = cy - L.barH / 2, w = 2 * reach, h = L.barH;
      int cr = h * 2 / 5;
      uint16_t *b = (uint16_t *)malloc((size_t)w * h * 2); if (!b) return;
      for (int ry = 0; ry < h; ry++) {
          uint16_t col = lerp565(0x33, 0x3c, 0x4b, 0x1a, 0x20, 0x29, (y0 + ry) - top, bot - top);
          for (int rx = 0; rx < w; rx++) b[ry * w + rx] = in_rrect(rx, ry, w, h, cr) ? col : bg;
      }
      chevron(b, w, h, 2, inset, h / 2, chw, chh, chev);            /* left  (apex at left tip)  */
      chevron(b, w, h, 3, w - 1 - inset, h / 2, chw, chh, chev);    /* right (apex at right tip) */
      board_lcd_blit(x0, y0, w, h, b); free(b);
    }
    /* vertical bar (rounded) + up/down chevrons + hub (drawn 2nd so it caps the cross centre) */
    { int x0 = cx - L.barW / 2, y0 = cy - reach, w = L.barW, h = 2 * reach;
      int cr = w * 2 / 5;
      uint16_t *b = (uint16_t *)malloc((size_t)w * h * 2); if (!b) return;
      for (int ry = 0; ry < h; ry++) {
          uint16_t col = lerp565(0x33, 0x3c, 0x4b, 0x1a, 0x20, 0x29, (y0 + ry) - top, bot - top);
          for (int rx = 0; rx < w; rx++) b[ry * w + rx] = in_rrect(rx, ry, w, h, cr) ? col : bg;
      }
      chevron(b, w, h, 0, w / 2, inset, chw, chh, chev);            /* up   (apex at top tip)    */
      chevron(b, w, h, 1, w / 2, h - 1 - inset, chw, chh, chev);    /* down (apex at bottom tip) */
      uint16_t hubc = board_lcd_rgb565(0x14, 0x19, 0x22);          /* subtle dark hub */
      int hub = L.dead * 7 / 8;
      for (int dy = -hub; dy <= hub; dy++) for (int dx = -hub; dx <= hub; dx++)
          if (dx * dx + dy * dy <= hub * hub) {
              int px = w / 2 + dx, py = h / 2 + dy;
              if (px >= 0 && px < w && py >= 0 && py < h) b[py * w + px] = hubc;
          }
      board_lcd_blit(x0, y0, w, h, b); free(b);
    }
}

static const char *const *glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    for (size_t i = 0; i < sizeof(PICO8_FONT_GLYPHS) / sizeof(PICO8_FONT_GLYPHS[0]); i++)
        if (PICO8_FONT_GLYPHS[i].c == c) return PICO8_FONT_GLYPHS[i].rows;
    return NULL;
}
static void text(int cx, int cy, const char *s, uint16_t fg, uint16_t bg, int sc) {
    int n = (int)strlen(s), cw = 4 * sc, tw = n * cw, th = 5 * sc;
    if (tw <= 0) return;
    uint16_t *b = (uint16_t *)malloc((size_t)tw * th * 2); if (!b) return;
    for (int i = 0; i < tw * th; i++) b[i] = bg;
    for (int ci = 0; ci < n; ci++) {
        const char *const *rows = glyph(s[ci]); if (!rows) continue;
        int ox = ci * cw;
        for (int ry = 0; ry < 5; ry++)
            for (int rx = 0; rx < 3; rx++)
                if (rows[ry][rx] == '#')
                    for (int yy = 0; yy < sc; yy++)
                        for (int xx = 0; xx < sc; xx++)
                            b[(ry * sc + yy) * tw + (ox + rx * sc + xx)] = fg;
    }
    board_lcd_blit(cx - tw / 2, cy - th / 2, tw, th, b); free(b);
}

static void draw_deck(void) {
    uint16_t bg    = deck_bg();
    uint16_t orng  = board_lcd_rgb565(0xe7, 0x9a, 0xa0);
    uint16_t xrng  = board_lcd_rgb565(0x5f, 0xc4, 0xbb);
    uint16_t menu  = board_lcd_rgb565(0x4a, 0x55, 0x66);
    uint16_t olbl  = board_lcd_rgb565(0xf3, 0xd6, 0xd8);
    uint16_t xlbl  = board_lcd_rgb565(0xcf, 0xee, 0xea);
    uint16_t mlbl  = board_lcd_rgb565(0x9a, 0xa6, 0xb6);
    draw_dpad();
    disc_sphere(L.o_cx, L.o_cy, L.btn_r, orng, bg, 'O', olbl);
    disc_sphere(L.x_cx, L.x_cy, L.btn_r, xrng, bg, 'X', xlbl);
    int mt = 1 + L.menu_h / 16;
    rrect_outline(L.menu_cx - L.menu_w / 2, L.menu_cy - L.menu_h / 2, L.menu_w, L.menu_h,
                  L.menu_h / 2, mt, menu, bg);
    text(L.menu_cx, L.menu_cy, "MENU", mlbl, bg, L.lbl > 1 ? L.lbl - 1 : 1);
    ESP_LOGI(TAG, "deck drawn (P4): dpad(%d,%d) O(%d,%d) X(%d,%d) menu(%d,%d) r=%d",
             L.dpad_cx, L.dpad_cy, L.o_cx, L.o_cy, L.x_cx, L.x_cy, L.menu_cx, L.menu_cy, L.btn_r);
}

/* One touch point (display coords) -> a single button bit (0 if it hits nothing). */
static uint8_t point_to_button(int x, int y) {
    int dx = x - L.dpad_cx, dy = y - L.dpad_cy;
    if (dx > -L.reach && dx < L.reach && dy > -L.reach && dy < L.reach) {
        if (sq(dx) + sq(dy) < sq(L.dead)) return 0;             /* dead hub */
        int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;     /* dominant axis picks the direction */
        if (ax > ay) return dx < 0 ? INPUT_LEFT : INPUT_RIGHT;
        return dy < 0 ? INPUT_UP : INPUT_DOWN;
    }
    int hit = L.btn_r + L.btn_r / 6;                            /* hit radius a touch larger than drawn */
    if (sq(x - L.o_cx) + sq(y - L.o_cy) < sq(hit)) return INPUT_O;
    if (sq(x - L.x_cx) + sq(y - L.x_cy) < sq(hit)) return INPUT_X;
    if (x >= L.menu_cx - L.menu_w / 2 && x <= L.menu_cx + L.menu_w / 2 &&
        y >= L.menu_cy - L.menu_h / 2 && y <= L.menu_cy + L.menu_h / 2) return INPUT_PAUSE;
    return 0;
}

} // namespace

extern "C" void    board_draw_touch_deck(void)        { layout_init(); draw_deck(); }
extern "C" uint8_t board_touch_hittest(int x, int y)  { return point_to_button(x, y); }

/* ===================== ES8311 audio output (GP-6) =====================
 * Onboard ES8311 codec over the shared touch I2C bus (0x18) + an I2S standard TX channel. Output only
 * (PICO-8 playback): 22050 Hz, 16-bit, stereo. Pins from docs/hardware/pico-e32-guition-jc4880p443c-p4.md:
 * MCLK13 / BCLK12 / WS(LRCLK)10 / DOUT(spk)9. Driven via the vendored esp_codec_dev component. */
#define AUDIO_MCLK   GPIO_NUM_13
#define AUDIO_BCLK   GPIO_NUM_12
#define AUDIO_WS     GPIO_NUM_10
#define AUDIO_DOUT   GPIO_NUM_9
#define AUDIO_PA     GPIO_NUM_11   /* speaker power-amp enable, active-high — WITHOUT this the DAC plays but
                                    * the amp is off and nothing is audible (giltal/RetroESP32-P4 pins_config.h) */
#define AUDIO_SR     22050

static i2s_chan_handle_t      s_i2s_tx = NULL;
static esp_codec_dev_handle_t s_codec  = NULL;

extern "C" esp_err_t board_audio_init(void) {
    if (ensure_i2c_bus() != ESP_OK) { ESP_LOGE(TAG, "audio: I2C bus"); return ESP_FAIL; }

    /* I2S standard TX channel (ESP is the master clock). */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL), TAG, "audio: i2s channel");
    i2s_std_config_t std = {};
    std.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SR);
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std.gpio_cfg.mclk = AUDIO_MCLK;
    std.gpio_cfg.bclk = AUDIO_BCLK;
    std.gpio_cfg.ws   = AUDIO_WS;
    std.gpio_cfg.dout = AUDIO_DOUT;
    std.gpio_cfg.din  = I2S_GPIO_UNUSED;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx, &std), TAG, "audio: i2s std");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG, "audio: i2s enable");

    /* ES8311 control (I2C) + data (I2S) interfaces via esp_codec_dev. */
    audio_codec_i2c_cfg_t i2c_cfg = {};
    i2c_cfg.port       = I2C_NUM_0;
    i2c_cfg.addr       = ES8311_CODEC_DEFAULT_ADDR;   /* 0x18 << 1 */
    i2c_cfg.bus_handle = s_i2c_bus;
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl_if) { ESP_LOGE(TAG, "audio: i2c ctrl"); return ESP_FAIL; }

    audio_codec_i2s_cfg_t i2s_data = {};
    i2s_data.port      = I2S_NUM_0;
    i2s_data.tx_handle = s_i2s_tx;
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_data);
    if (!data_if) { ESP_LOGE(TAG, "audio: i2s data"); return ESP_FAIL; }

    es8311_codec_cfg_t es_cfg = {};
    es_cfg.ctrl_if     = ctrl_if;
    es_cfg.gpio_if     = audio_codec_new_gpio();
    es_cfg.codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC;   /* output only */
    es_cfg.master_mode = false;                         /* ESP drives BCLK/WS */
    es_cfg.use_mclk    = true;
    es_cfg.pa_pin      = AUDIO_PA;                      /* the es8311 driver drives this high to enable the amp */
    es_cfg.pa_reverted = false;                         /* active-high */
    es_cfg.mclk_div    = 256;
    const audio_codec_if_t *es_if = es8311_codec_new(&es_cfg);
    if (!es_if) { ESP_LOGE(TAG, "audio: es8311 new"); return ESP_FAIL; }

    esp_codec_dev_cfg_t dev_cfg = {};
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    dev_cfg.codec_if = es_if;
    dev_cfg.data_if  = data_if;
    s_codec = esp_codec_dev_new(&dev_cfg);
    if (!s_codec) { ESP_LOGE(TAG, "audio: codec dev"); return ESP_FAIL; }

    esp_codec_dev_sample_info_t si = {};
    si.bits_per_sample = 16;
    si.channel         = 2;
    si.channel_mask    = 0x03;
    si.sample_rate     = AUDIO_SR;
    if (esp_codec_dev_open(s_codec, &si) != ESP_CODEC_DEV_OK) { ESP_LOGE(TAG, "audio: codec open"); return ESP_FAIL; }
    esp_codec_dev_set_out_vol(s_codec, 40);   /* 0..100 — 70 clipped/too loud on the onboard speaker */
    ESP_LOGI(TAG, "ES8311 audio up: %d Hz S16 stereo (MCLK%d BCLK%d WS%d DOUT%d PA%d)",
             AUDIO_SR, AUDIO_MCLK, AUDIO_BCLK, AUDIO_WS, AUDIO_DOUT, AUDIO_PA);
    return ESP_OK;
}

/* Play `frames` stereo S16 frames (4 bytes each). esp_codec_dev_write blocks until the I2S DMA accepts it,
 * which self-paces the caller (fake-08's game loop) to the 22050 Hz audio clock. */
/* Play `frames` stereo S16 frames (4 bytes each). esp_codec_dev_write blocks until the I2S DMA accepts it,
 * which self-paces the caller — the core-1 audio_task — to the 22050 Hz audio clock. */
extern "C" void board_audio_write(const int16_t *stereo, size_t frames) {
    if (!s_codec || !stereo || frames == 0) return;
    esp_codec_dev_write(s_codec, (void *)stereo, frames * 4);
}

/* --- TF/microSD over the ESP32-P4 SDMMC peripheral (slot 0, 4-bit). --- */
/* The P4 routes SDMMC through the GPIO matrix, so the slot pins are set explicitly (not fixed IO_MUX).
 * These are the JC4880P443C TF-slot pins: CLK43 CMD44 D0..D3=39..42. The card supplies its own pull-ups on
 * a populated board; SDMMC_SLOT_FLAG_INTERNAL_PULLUP is a belt-and-suspenders for cards/wiring that don't. */
#define PIN_SD_CLK  GPIO_NUM_43
#define PIN_SD_CMD  GPIO_NUM_44
#define PIN_SD_D0   GPIO_NUM_39
#define PIN_SD_D1   GPIO_NUM_40
#define PIN_SD_D2   GPIO_NUM_41
#define PIN_SD_D3   GPIO_NUM_42

/* CRITICAL for the P4: the SD card's 3.3V rail is NOT always-on — it's fed by the ESP32-P4's on-chip LDO
 * channel VO4, which must be brought up (as host.pwr_ctrl_handle) BEFORE mounting. Without it the card is
 * unpowered and every init command (CMD0/CMD8/ACMD41) times out (ESP_ERR_TIMEOUT / send_op_cond 0x107) even
 * though the pins are correct. Channel 4 + the pin map (43/44/39-42) are from giltal/RetroESP32-P4
 * (components/odroid/odroid_sdcard.c), the same reference that gave us the ES8311 PA-enable. */
#define SD_LDO_CHAN_ID 4

static sdmmc_card_t       *s_sd_card = nullptr;
static sd_pwr_ctrl_handle_t s_sd_pwr = nullptr;

static esp_err_t sd_try_mount(const char *mount_point, int width, sd_pwr_ctrl_handle_t pwr) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;   /* matches the RetroESP32-P4 reference */
    host.pwr_ctrl_handle = pwr;                 /* the on-chip LDO that powers the card's rail */

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = width;                 /* 4 = D0-D3; 1 = D0 only (fewer lines that must be good) */
    slot.clk = PIN_SD_CLK;
    slot.cmd = PIN_SD_CMD;
    slot.d0  = PIN_SD_D0;
    slot.d1  = PIN_SD_D1;
    slot.d2  = PIN_SD_D2;
    slot.d3  = PIN_SD_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;   /* this board has no external CMD/DAT pull-ups */

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {};
    mcfg.format_if_mount_failed = false;   /* never reformat the user's card */
    mcfg.max_files              = 5;
    mcfg.allocation_unit_size   = 16 * 1024;

    return esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot, &mcfg, &s_sd_card);
}

/* 480x800 panel; the fake-08 host runs the game at 128*3 = 384, centred (x=48). Thumbnail is a portrait
 * cover tile that sits in the top region above the deck with room for the breadcrumb + position bar. */
extern "C" void board_carousel_layout(board_carousel_layout_t *out) {
    out->game_x = 48;  out->game_w = 384;
    out->thumb_w = 236; out->thumb_h = 302; out->thumb_y = 46;
    out->side_w = 64;  out->crumb_y = 16;
    out->title_y = 96; out->title_scale = 6; out->body_y = 200; out->body_dy = 62;
    out->body_scale = 6;   /* big MIPI panel: match the title weight so the items read large on the tall 800px screen */
    out->info_scale = 3;   /* big MIPI panel: scale-3 Settings/About body text (scale 2 read too small) */
}

extern "C" esp_err_t board_sd_mount(const char *mount_point) {
    if (s_sd_card) return ESP_ERR_INVALID_STATE;   /* already mounted */

    /* Power the SD rail via the on-chip LDO (VO4, 3.3V) — REQUIRED before any SDMMC command reaches the card. */
    if (!s_sd_pwr) {
        sd_pwr_ctrl_ldo_config_t ldo = {};
        ldo.ldo_chan_id = SD_LDO_CHAN_ID;
        esp_err_t lerr = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_sd_pwr);
        if (lerr != ESP_OK) {
            ESP_LOGW(TAG, "SD LDO (VO%d) init failed (%s) — SD unpowered, mount will fail",
                     SD_LDO_CHAN_ID, esp_err_to_name(lerr));
            s_sd_pwr = nullptr;
        }
    }

    /* Try 4-bit first (the reference config), then 1-bit as a fallback (a flaky D1-D3 line can time out 4-bit
     * init while 1-bit — CLK/CMD/D0 only — still comes up). Power-cycle the LDO before the 1-bit retry. */
    int width = 4;
    esp_err_t err = sd_try_mount(mount_point, 4, s_sd_pwr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD 4-bit mount failed (%s); power-cycling LDO + retrying 1-bit", esp_err_to_name(err));
        s_sd_card = nullptr;
        if (s_sd_pwr) {   /* power-cycle the card, like the reference does between attempts */
            sd_pwr_ctrl_del_on_chip_ldo(s_sd_pwr);
            s_sd_pwr = nullptr;
            vTaskDelay(pdMS_TO_TICKS(300));
            sd_pwr_ctrl_ldo_config_t ldo = {};
            ldo.ldo_chan_id = SD_LDO_CHAN_ID;
            if (sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_sd_pwr) != ESP_OK) s_sd_pwr = nullptr;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        width = 1;
        err = sd_try_mount(mount_point, 1, s_sd_pwr);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed (%s) — no card, card not seated, or wiring/format issue",
                 esp_err_to_name(err));
        s_sd_card = nullptr;
        return err;
    }
    ESP_LOGI(TAG, "SD mounted at %s (%lluMB, %d-bit, CLK%d CMD%d D0-3=%d/%d/%d/%d, LDO VO%d)",
             mount_point, ((uint64_t)s_sd_card->csd.capacity) * s_sd_card->csd.sector_size / (1024 * 1024),
             width, PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0, PIN_SD_D1, PIN_SD_D2, PIN_SD_D3, SD_LDO_CHAN_ID);
    return ESP_OK;
}

/* Release the card AND the SDMMC host, so esp-hosted can claim the host for the ESP32-C6 (WC-6). The launcher
 * calls this around a WiFi session; the host is handed back by board_sd_mount() afterwards.
 *
 * Do NOT add an sdmmc_host_deinit() here. esp_vfs_fat_sdcard_unmount() already deinitialises the host and frees
 * the card (IDF components/fatfs/vfs/vfs_fat_sdmmc.c: unmount_card_core -> call_host_deinit + free); calling it
 * again is a double-deinit that panics the board into a boot loop. The LDO VO4 rail is deliberately left up —
 * it powers the card itself, is needed again on remount, and takes ~300 ms to settle if torn down. */
extern "C" esp_err_t board_sd_unmount(const char *mount_point) {
    if (!s_sd_card) return ESP_ERR_INVALID_STATE;
    esp_err_t err = esp_vfs_fat_sdcard_unmount(mount_point, s_sd_card);
    s_sd_card = nullptr;
    ESP_LOGI(TAG, "SD unmounted, SDMMC host released -> %s", esp_err_to_name(err));
    return err;
}
