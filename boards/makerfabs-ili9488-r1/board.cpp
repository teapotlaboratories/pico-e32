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
#include "input.h"                 /* INPUT_* button bits, for board_touch_hittest */
#include <string.h>
#include <stdio.h>                 /* snprintf (fps HUD) */

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "driver/ledc.h"
#include "driver/i2c_master.h"   /* FT6236 touch (new IDF I2C master API) */
#include "driver/gpio.h"
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

/* ---- THIS BOARD'S microSD WIRING (rev 1) — a PRIVATE SPI2 bus, disjoint from the i80 LCD ----
 * The LCD is 16-bit i80 parallel (PIN_WR/DC/CS + DATA_PINS above) and uses NO SPI host, so SPI2 is
 * free with zero pin overlap: the SD owns SPI2 exclusively (owns_bus=true). The old "microSD shared
 * with the LCD" note was a 4"-board carry-over (DP-7); wrong for this board. SPI2_HOST and GPIO_NUM_*
 * come in via board.h -> sdcard_spi.h. The mount MECHANISM + policy live in components/sdcard_spi and
 * the app; here we own only the wiring, next to the LCD's. */
#define SD_PIN_CS   GPIO_NUM_1
#define SD_PIN_MOSI GPIO_NUM_2
#define SD_PIN_MISO GPIO_NUM_41
#define SD_PIN_CLK  GPIO_NUM_42

/* ---- THIS BOARD'S TOUCH WIRING — FT6236 capacitive, I2C (INT/RST are NC, so we poll) ---- */
#define TOUCH_SDA      GPIO_NUM_38
#define TOUCH_SCL      GPIO_NUM_39
#define TOUCH_ADDR     0x38
#define TOUCH_I2C_PORT I2C_NUM_0

/* ORIENTATION: no rotation. The panel reads upright at offset_rotation=0 for this board as mounted,
 * confirmed by eye on the bench (2026-07-29). This flag couples the display AND the touch transform: when
 * true it applies a full 180° (offset_rotation=2, MADCTL MX|MY, MV clear) and flips both touch axes to
 * match; when false both pass through unrotated. History — an earlier revision ran this at 180° on the
 * theory that the glass was mounted upside-down; note the bench camera adds its own horizontal mirror, so
 * trust the panel by eye, not captures, when re-checking. docs/worklog/2026-07-16-yflip-and-gate1-fps.md. */
static const bool ROTATE_180 = false;

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
            p.offset_rotation = ROTATE_180 ? 2 : 0;   /* full 180° — see ROTATE_180 above */
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
    /* Zero-init then assign, rather than a partial designated initializer: the LEDC config structs gained
     * fields across IDF versions (v6 added ledc_timer_config_t::deconfigure and ledc_channel_config_t::
     * intr_type/sleep_mode/flags), and a partial initializer trips -Werror=missing-field-initializers on v6.
     * `= {}` zero-fills every field (including ones we don't set) and is clean on both v5.4.2 and v6.0.2. */
    ledc_timer_config_t t = {};
    t.speed_mode      = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = LEDC_TIMER_8_BIT;
    t.timer_num       = LEDC_TIMER_0;
    t.freq_hz         = 5000;
    t.clk_cfg         = LEDC_AUTO_CLK;
    ledc_timer_config(&t);
    ledc_channel_config_t c = {};
    c.gpio_num   = pin;
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel    = LEDC_CHANNEL_0;
    c.timer_sel  = LEDC_TIMER_0;
    c.duty       = 255;
    c.hpoint     = 0;
    ledc_channel_config(&c);
}

extern "C" esp_err_t board_lcd_init(void) {
    s_lcd = new PanelLGFX();
    if (!s_lcd) return ESP_ERR_NO_MEM;
    if (!s_lcd->init()) { ESP_LOGE(TAG, "LovyanGFX init failed"); return ESP_FAIL; }
    backlight_on(PIN_BL);
    ESP_LOGI(TAG, "LCD up via LovyanGFX (%dx%d, %d Hz, cs=%d, rot180=%d, dlen_16bit)",
             BOARD_LCD_H_RES, BOARD_LCD_V_RES, BOARD_LCD_PCLK_HZ, PIN_CS, ROTATE_180);
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

extern "C" int board_lcd_width(void)  { return BOARD_LCD_H_RES; }
extern "C" int board_lcd_height(void) { return BOARD_LCD_V_RES; }

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

/* This board's microSD wiring — a private SPI2 bus (see the SD_PIN_* block above). Writes only the
 * hardware fields; the app keeps its mount policy. Symmetric with board_lcd_init: the board owns its
 * SD wiring the same way it owns its display. */
extern "C" bool board_sd_config(sdcard_spi_config_t *out) {
    out->host     = SPI2_HOST;
    out->pin_cs   = SD_PIN_CS;
    out->pin_mosi = SD_PIN_MOSI;
    out->pin_miso = SD_PIN_MISO;
    out->pin_sclk = SD_PIN_CLK;
    out->owns_bus = true;   /* SD owns SPI2 exclusively on this board */
    return true;
}

/* ---- FT6236 capacitive touch (I2C). The board owns the controller AND the orientation, so
 * board_touch_read returns points already in display coordinates (ROTATE_180 applied). Register map is
 * the standard FocalTech FT6x36 layout (TD_STATUS at 0x02; point 1 at 0x03..0x06, point 2 at 0x09..0x0C;
 * X/Y are 12-bit, high nibble in the *H byte). Refs: FT6236 datasheet; common ft6x36 drivers. ---- */
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_touch   = NULL;

extern "C" esp_err_t board_touch_init(void) {
    i2c_master_bus_config_t bus = {};
    bus.i2c_port                     = TOUCH_I2C_PORT;
    bus.sda_io_num                   = TOUCH_SDA;
    bus.scl_io_num                   = TOUCH_SCL;
    bus.clk_source                   = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt            = 7;
    bus.flags.enable_internal_pullup = true;
    esp_err_t r = i2c_new_master_bus(&bus, &s_i2c_bus);
    if (r != ESP_OK) { ESP_LOGW(TAG, "touch: i2c bus init failed: %s", esp_err_to_name(r)); return r; }

    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address  = TOUCH_ADDR;
    dev.scl_speed_hz    = 100000;   /* 100 kHz — robust on this board's polled bus */
    r = i2c_master_bus_add_device(s_i2c_bus, &dev, &s_touch);
    if (r != ESP_OK) { ESP_LOGW(TAG, "touch: add FT6236 failed: %s", esp_err_to_name(r)); return r; }

    /* Probe the chip-id register (0xA3). A NAK here points at wiring or the I2C address, not the code. */
    uint8_t reg = 0xA3, id = 0xFF;
    r = i2c_master_transmit_receive(s_touch, &reg, 1, &id, 1, pdMS_TO_TICKS(100));
    if (r == ESP_OK) {
        ESP_LOGI(TAG, "FT6236 touch up (chip id 0x%02x) — I2C SDA=%d SCL=%d addr=0x%02x",
                 id, TOUCH_SDA, TOUCH_SCL, TOUCH_ADDR);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "FT6236 not responding at 0x%02x (%s) — touch disabled", TOUCH_ADDR, esp_err_to_name(r));
    return r;
}

/* Read one FT6236 register (single-byte transaction — the shape the boot probe proved works; the chip
 * NAKs a multi-byte burst read here). */
static esp_err_t ft_rd(uint8_t reg, uint8_t *val) {
    return i2c_master_transmit_receive(s_touch, &reg, 1, val, 1, pdMS_TO_TICKS(100));
}

extern "C" int board_touch_read(int *xs, int *ys, int max) {
    if (!s_touch || max <= 0) return 0;
    uint8_t td = 0;
    if (ft_rd(0x02, &td) != ESP_OK) return 0;   /* TD_STATUS: active touch count in the low nibble */
    int n = td & 0x0F;
    if (n > 2)   n = 2;
    if (n > max) n = max;
    int out = 0;
    for (int i = 0; i < n; ++i) {
        uint8_t base = (i == 0) ? 0x03 : 0x09;  /* P1 regs 0x03..0x06, P2 regs 0x09..0x0C */
        uint8_t xh, xl, yh, yl;
        if (ft_rd(base, &xh) != ESP_OK || ft_rd(base + 1, &xl) != ESP_OK ||
            ft_rd(base + 2, &yh) != ESP_OK || ft_rd(base + 3, &yl) != ESP_OK) continue;
        int rx = ((xh & 0x0F) << 8) | xl;
        int ry = ((yh & 0x0F) << 8) | yl;
        /* Orientation: stay coupled to the display (see ROTATE_180 in the LCD). At 180° flip BOTH axes so a
         * touch lands where its control is drawn; unrotated, pass through. */
        xs[out] = ROTATE_180 ? (BOARD_LCD_H_RES - 1) - rx : rx;
        ys[out] = ROTATE_180 ? (BOARD_LCD_V_RES - 1) - ry : ry;
        ++out;
    }
    return out;
}

/* ===================== On-screen touch control deck — S3-owned layout + rendering =====================
 * Drawn with LovyanGFX VECTOR primitives (fillRoundRect / fillCircle / drawCircle / fillTriangle), NOT a
 * rasterised buffer blit. That matters: this board's i80 bus (Bus_Parallel16) drives an odd-total-pixel
 * pushImage into an unbounded DMA-FIFO wait, so blitting a 2r+1 square (odd) wedges the whole task — the
 * vector calls sidestep it entirely (the way this board's original deck did). Geometry for the 320x480
 * panel (game = top 256, deck below); draw + hit-test share these constants so they stay consistent.
 * Approximates docs/runtime/pico-e32-fake08-touch-ui.html with native primitives. */
namespace {
struct DeckGeom {
    static const int GAME_H = 256;
    static const int DPAD_CX = 92,  DPAD_CY = 376, REACH = 70, DEAD = 16;
    static const int BAR_L = 140, BAR_W = 50, BAR_R = 20;
    static const int O_CX = 212, O_CY = 414, X_CX = 272, X_CY = 352, BTN_R = 31;
    static const int MENU_X = 131, MENU_Y = 272, MENU_W = 58, MENU_H = 22, MENU_R = 11;
};
static inline int sq_(int v) { return v * v; }
} // namespace

extern "C" void board_draw_touch_deck(void) {
    if (!s_lcd) return;
    auto &g = *s_lcd;
    typedef DeckGeom D;
    g.startWrite();
    /* deck surface — subtle dark blue-grey (matches ESP32Host's one-time fill + the P4 deck), not black */
    g.fillRect(0, D::GAME_H, BOARD_LCD_H_RES, BOARD_LCD_V_RES - D::GAME_H, g.color888(0x0f, 0x14, 0x1d));
    /* d-pad: two rounded bars form the cross + soft-blue chevrons + a dark hub */
    uint32_t bar  = g.color888(0x2b, 0x33, 0x41);
    uint32_t chev = g.color888(0x7f, 0xa8, 0xe6);
    g.fillRoundRect(D::DPAD_CX - D::BAR_L / 2, D::DPAD_CY - D::BAR_W / 2, D::BAR_L, D::BAR_W, D::BAR_R, bar);
    g.fillRoundRect(D::DPAD_CX - D::BAR_W / 2, D::DPAD_CY - D::BAR_L / 2, D::BAR_W, D::BAR_L, D::BAR_R, bar);
    g.fillTriangle(D::DPAD_CX, D::DPAD_CY - 56, D::DPAD_CX - 8, D::DPAD_CY - 46, D::DPAD_CX + 8, D::DPAD_CY - 46, chev); /* up   */
    g.fillTriangle(D::DPAD_CX, D::DPAD_CY + 56, D::DPAD_CX - 8, D::DPAD_CY + 46, D::DPAD_CX + 8, D::DPAD_CY + 46, chev); /* down */
    g.fillTriangle(D::DPAD_CX - 56, D::DPAD_CY, D::DPAD_CX - 46, D::DPAD_CY - 8, D::DPAD_CX - 46, D::DPAD_CY + 8, chev); /* left */
    g.fillTriangle(D::DPAD_CX + 56, D::DPAD_CY, D::DPAD_CX + 46, D::DPAD_CY - 8, D::DPAD_CX + 46, D::DPAD_CY + 8, chev); /* right*/
    g.fillCircle(D::DPAD_CX, D::DPAD_CY, D::DEAD - 2, g.color888(0x14, 0x19, 0x22));                                    /* hub  */
    /* O / X — spheres (base + upper highlight + thin coloured ring) with a vector glyph, gamepad diagonal */
    const struct { int cx, cy; uint32_t ring, lbl; char k; } btn[2] = {
        { D::O_CX, D::O_CY, g.color888(0xe7, 0x9a, 0xa0), g.color888(0xf3, 0xd6, 0xd8), 'O' },
        { D::X_CX, D::X_CY, g.color888(0x5f, 0xc4, 0xbb), g.color888(0xcf, 0xee, 0xea), 'X' },
    };
    for (int i = 0; i < 2; i++) {
        int cx = btn[i].cx, cy = btn[i].cy, r = D::BTN_R;
        g.fillCircle(cx, cy, r, g.color888(0x22, 0x29, 0x33));                       /* base                  */
        g.fillCircle(cx, cy - r * 35 / 100, r * 62 / 100, g.color888(0x2f, 0x37, 0x43)); /* upper highlight   */
        g.drawCircle(cx, cy, r, btn[i].ring); g.drawCircle(cx, cy, r - 1, btn[i].ring); /* thin coloured ring */
        if (btn[i].k == 'O') {                                                       /* O glyph: a ring       */
            int rg = r * 42 / 100;
            g.drawCircle(cx, cy, rg, btn[i].lbl); g.drawCircle(cx, cy, rg - 1, btn[i].lbl);
        } else {                                                                     /* X glyph: a cross      */
            int q = r * 40 / 100;
            for (int t = -1; t <= 1; t++) {
                g.drawLine(cx - q, cy - q + t, cx + q, cy + q + t, btn[i].lbl);
                g.drawLine(cx - q, cy + q + t, cx + q, cy - q + t, btn[i].lbl);
            }
        }
    }
    /* MENU: outline pill (fill:none) + muted centred label */
    g.drawRoundRect(D::MENU_X, D::MENU_Y, D::MENU_W, D::MENU_H, D::MENU_R, g.color888(0x4a, 0x55, 0x66));
    g.setTextSize(1);
    g.setTextColor(g.color888(0x9a, 0xa6, 0xb6));
    g.drawString("MENU", D::MENU_X + D::MENU_W / 2 - 12, D::MENU_Y + D::MENU_H / 2 - 4);
    g.endWrite();
    ESP_LOGI(TAG, "touch deck drawn (S3 LovyanGFX-native): dpad(%d,%d) O(%d,%d) X(%d,%d) menu(%d,%d) r=%d",
             D::DPAD_CX, D::DPAD_CY, D::O_CX, D::O_CY, D::X_CX, D::X_CY,
             D::MENU_X + D::MENU_W / 2, D::MENU_Y + D::MENU_H / 2, D::BTN_R);
}

/* One touch point (display coords) -> a single INPUT_* bit (0 if it hits nothing). Same geometry as the
 * draw above, so hits land on what the eye sees. */
extern "C" uint8_t board_touch_hittest(int x, int y) {
    typedef DeckGeom D;
    int dx = x - D::DPAD_CX, dy = y - D::DPAD_CY;
    if (dx > -D::REACH && dx < D::REACH && dy > -D::REACH && dy < D::REACH) {
        if (sq_(dx) + sq_(dy) < sq_(D::DEAD)) return 0;             /* dead hub */
        int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;        /* dominant axis picks the direction */
        if (ax > ay) return dx < 0 ? INPUT_LEFT : INPUT_RIGHT;
        return dy < 0 ? INPUT_UP : INPUT_DOWN;
    }
    int hit = D::BTN_R + D::BTN_R / 6;                             /* hit radius a touch larger than drawn */
    if (sq_(x - D::O_CX) + sq_(y - D::O_CY) < sq_(hit)) return INPUT_O;
    if (sq_(x - D::X_CX) + sq_(y - D::X_CY) < sq_(hit)) return INPUT_X;
    if (x >= D::MENU_X && x <= D::MENU_X + D::MENU_W && y >= D::MENU_Y && y <= D::MENU_Y + D::MENU_H) return INPUT_PAUSE;
    return 0;
}

/* Audio seam — this board has no onboard audio hardware. board_audio_init() fails (the Host then runs
 * silent) and board_audio_write() is a no-op, so the shared ESP32Host still links on this board. */
extern "C" esp_err_t board_audio_init(void) { return ESP_FAIL; }
extern "C" void      board_audio_write(const int16_t *stereo, size_t frames) { (void)stereo; (void)frames; }

/* The dev FPS HUD (board_lcd_draw_fps) and its g_hud_owned_by_app flag moved to the shared, PORTABLE HUD at
 * firmware/pico-e32-fake08/main/fps_hud.cpp — it renders through board_lcd_blit + board_lcd_rgb565, so one
 * implementation serves every board (this ILI9488 board and the P4 ST7701S board) with no per-board text code. */
