/* pico-e32-p4-hello — GP-2 proof-of-life for the Guition JC4880P443C (ESP32-P4).
 *
 * Minimal, board-agnostic: dumps the chip/flash/PSRAM identity over serial, exercises the board.h
 * display seam (a stub on this board in GP-2), then heartbeats. Its only job is to prove the P4
 * build -> flash -> boot path end to end and confirm the toolchain, PSRAM, and console wiring —
 * NOT to drive the panel (that is GP-3). See docs/hardware/pico-e32-guition-jc4880p443c-p4.md.
 */
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include <stdlib.h>
#include "board.h"
#include "input.h"

static const char *TAG = "p4-hello";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "==== pico-e32 P4 proof-of-life (GP-2) ====");
    ESP_LOGI(TAG, "IDF version: %s", esp_get_idf_version());

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    /* .revision is MXX = wafer_major*100 + wafer_minor (e.g. 103 -> v1.3). */
    ESP_LOGI(TAG, "target=%s  cores=%d  silicon_rev=v%d.%d",
             CONFIG_IDF_TARGET, (int)chip.cores,
             chip.revision / 100, chip.revision % 100);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "flash: %u MB", (unsigned)(flash_size / (1024 * 1024)));
    } else {
        ESP_LOGW(TAG, "flash: size read failed");
    }

    size_t psram = esp_psram_get_size();
    ESP_LOGI(TAG, "PSRAM: initialized=%d  size=%u bytes (%u MB)",
             (int)esp_psram_is_initialized(), (unsigned)psram,
             (unsigned)(psram / (1024 * 1024)));

    ESP_LOGI(TAG, "heap free: internal=%u B  PSRAM=%u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* Bring up the board display (GP-3: ST7701S over MIPI-DSI). */
    esp_err_t r = board_lcd_init();
    ESP_LOGI(TAG, "board_lcd_init -> %s   (panel %dx%d)",
             esp_err_to_name(r), BOARD_LCD_H_RES, BOARD_LCD_V_RES);

    /* First-light test (GP-4 style): full R/G/B fills to prove colour, then black with four
     * distinct corner markers to prove geometry + orientation on the bench camera:
     *   RED top-left · GREEN top-right · BLUE bottom-left · WHITE bottom-right. */
    if (r == ESP_OK) {
        struct { uint8_t rr, gg, bb; const char *name; } fills[] = {
            {255, 0, 0, "RED"}, {0, 255, 0, "GREEN"}, {0, 0, 255, "BLUE"},
        };
        for (auto &f : fills) {
            ESP_LOGI(TAG, "fill %s", f.name);
            board_lcd_fill(board_lcd_rgb565(f.rr, f.gg, f.bb));
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
        board_lcd_fill(board_lcd_rgb565(0, 0, 0));
        const int M = 120;
        uint16_t *sq = (uint16_t *)malloc((size_t)M * M * 2);
        if (sq) {
            struct { int x, y; uint8_t rr, gg, bb; } corners[] = {
                {0, 0, 255, 0, 0},                                              /* TL red   */
                {BOARD_LCD_H_RES - M, 0, 0, 255, 0},                            /* TR green  */
                {0, BOARD_LCD_V_RES - M, 0, 0, 255},                            /* BL blue   */
                {BOARD_LCD_H_RES - M, BOARD_LCD_V_RES - M, 255, 255, 255},      /* BR white  */
            };
            for (auto &c : corners) {
                uint16_t col = board_lcd_rgb565(c.rr, c.gg, c.bb);
                for (int i = 0; i < M * M; i++) sq[i] = col;
                board_lcd_blit(c.x, c.y, M, M, sq);
            }
            free(sq);
        }
        ESP_LOGI(TAG, "first-light test drawn: RGB fills + corner markers (TL=R TR=G BL=B BR=W)");
    }

    /* GP-5: bring up GT911 touch and report points over serial (HITL: touch the panel to verify). */
    bool touch_ok = (board_touch_init() == ESP_OK);
    ESP_LOGI(TAG, "board_touch_init -> %s", touch_ok ? "OK (touch the panel to test)" : "unavailable");

    /* Serial-input demo (ported input backend): a marker on the panel moves with l/r/u/d and changes
     * colour on O(z)/X. Drive it over the console — no physical touch needed — and watch it on the camera:
     *   press 'r' -> the square jumps right; 'z' -> green; 'x' -> red. Verifies input end to end. */
    bool input_ok = false;
    const int MK = 80;                       /* marker size */
    uint16_t *mk = NULL;
    int mx = (BOARD_LCD_H_RES - MK) / 2, my = (BOARD_LCD_V_RES - MK) / 2, pmx = mx, pmy = my;
    if (r == ESP_OK) {
        input_ok = (input_init() == ESP_OK);
        ESP_LOGI(TAG, "input backend '%s' -> %s", input_backend_name(), input_ok ? "OK" : "unavailable");
        if (input_ok) {
            board_lcd_fill(board_lcd_rgb565(0, 0, 0));
            mk = (uint16_t *)malloc((size_t)MK * MK * 2);
        }
    }
    auto draw_marker = [&](int x, int y, uint16_t col) {
        if (!mk) return;
        for (int i = 0; i < MK * MK; i++) mk[i] = col;
        board_lcd_blit(x, y, MK, MK, mk);
    };
    uint16_t black = board_lcd_rgb565(0, 0, 0);
    if (mk) draw_marker(mx, my, board_lcd_rgb565(0, 255, 255));

    uint32_t n = 0;
    uint8_t last_held = 0xFF;
    int64_t next_beat = 0;
    for (;;) {
        if (input_ok) {
            uint8_t held = input_poll();
            const int STEP = 20;
            if (held & INPUT_LEFT)  mx -= STEP;
            if (held & INPUT_RIGHT) mx += STEP;
            if (held & INPUT_UP)    my -= STEP;
            if (held & INPUT_DOWN)  my += STEP;
            if (mx < 0) mx = 0;
            if (mx > BOARD_LCD_H_RES - MK) mx = BOARD_LCD_H_RES - MK;
            if (my < 0) my = 0;
            if (my > BOARD_LCD_V_RES - MK) my = BOARD_LCD_V_RES - MK;
            uint16_t col = (held & INPUT_O) ? board_lcd_rgb565(0, 255, 0)
                         : (held & INPUT_X) ? board_lcd_rgb565(255, 0, 0)
                                            : board_lcd_rgb565(0, 255, 255);
            if (mk && (mx != pmx || my != pmy || held != last_held)) {
                draw_marker(pmx, pmy, black);        /* erase old */
                draw_marker(mx, my, col);            /* draw new */
                pmx = mx; pmy = my;
            }
            if (held != last_held) {
                ESP_LOGI(TAG, "input held=0x%02X  marker=(%d,%d)", held, mx, my);
                last_held = held;
            }
        }
        if (touch_ok) {
            int xs[5], ys[5];
            int np = board_touch_read(xs, ys, 5);
            for (int i = 0; i < np; i++) {
                ESP_LOGI(TAG, "touch[%d/%d] x=%d y=%d", i + 1, np, xs[i], ys[i]);
            }
        }
        int64_t now = esp_timer_get_time();
        if (now >= next_beat) {
            next_beat = now + 1000000;   /* ~1 s */
            ESP_LOGI(TAG, "alive #%" PRIu32 "  uptime=%llus  free_heap=%u B",
                     n++, (unsigned long long)(now / 1000000), (unsigned)esp_get_free_heap_size());
        }
        vTaskDelay(pdMS_TO_TICKS(30));   /* ~33 Hz poll */
    }
}
