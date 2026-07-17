/* pico-e32-fake08 — app_main: boot fake-08's PICO-8 runtime and render a flash-embedded cart on the panel.
 *
 * This ESP-IDF entry replaces fake-08's desktop source/main.cpp: the same boot sequence, then a hand-off
 * to fake-08's OWN Vm::GameLoop() (the 1-to-1 port — the frame loop is fake-08's, not ours). The Host
 * binding (ESP32Host.cpp, in components/fake08/platform/esp32) draws through the board's board_lcd_*.
 *
 * Draw-only milestone: a tiny hand-written .p8 test cart (ours, not copyrighted) proves boot + render +
 * palette + font; input and audio are stubbed (parts-blocked). See docs/runtime/pico-e32-fake08-port.md.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "board.h"    /* board_lcd_init() — the app owns board bring-up */
#include "host.h"
#include "vm.h"
#include "PicoRam.h"
#include "Audio.h"
#include "logger.h"

static const char *TAG = "fake08";

/* Cart source. Default build = the tiny test cart below (ours). An opt-in CELESTE build
 * (`make build … DEFS='-D CELESTE=1'`) instead embeds the real Celeste .p8 from the gitignored
 * assets/celeste_p8.h — to exercise the full draw path (spr/map/print). Copyrighted; never committed. */
#if defined(CELESTE) && __has_include("celeste_p8.h")
#include "celeste_p8.h"
#define CART_BYTES CELESTE_P8
#define CART_LEN   CELESTE_P8_LEN
#define CART_NAME  "celeste.p8"
#else
#define CART_BYTES ((const unsigned char *)TEST_CART)
#define CART_LEN   (sizeof(TEST_CART) - 1)
#define CART_NAME  "test cart"
/* A minimal PICO-8 cart (ours): 16 colour bars + text + a frame counter. The Cart ctor detects the
 * "pico" magic and parses this as .p8 text (loadCartFromString). Exercises cls / rectfill / print and
 * both _update (the counter) and _draw. No __gfx__ needed — print() uses fake-08's built-in font. */
static const char TEST_CART[] =
    "pico-8 cartridge version 41\n"
    "__lua__\n"
    "t=0\n"
    "function _update()\n"
    " t+=1\n"
    "end\n"
    "function _draw()\n"
    " cls(1)\n"
    " for i=0,15 do\n"
    "  rectfill(i*8,0,i*8+7,127,i)\n"
    " end\n"
    " print(\"fake-08 on esp32-s3\",4,58,7)\n"
    " print(\"frame \"..t,4,68,7)\n"
    "end\n";
#endif

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "fake-08 port: draw-only milestone booting");

    if (board_lcd_init() != ESP_OK) {
        ESP_LOGE(TAG, "board_lcd_init failed");
        return;
    }

    /* fake-08 boot sequence (mirrors source/main.cpp:39-51). */
    Host    *host   = new Host(0, 0);
    PicoRam *memory = new PicoRam();
    memory->Reset();
    Audio   *audio  = new Audio(memory);
    Logger_Initialize(host->logFilePrefix());
    Vm      *vm     = new Vm(host, memory, nullptr, nullptr, audio);

    host->setUpPaletteColors();  /* must precede oneTimeSetup — it builds the RGB565 LUT */
    host->oneTimeSetup(audio);
    host->setTargetFps(30);
    vm->SetCartList(host->listcarts());

    ESP_LOGI(TAG, "loading %s (%u bytes)", CART_NAME, (unsigned)CART_LEN);
    if (!vm->LoadCart(CART_BYTES, CART_LEN, false)) {
        ESP_LOGE(TAG, "LoadCart failed");
        return;
    }
    vm->vm_run(); /* starts the cart coroutine */

    ESP_LOGI(TAG, "entering GameLoop");
    vm->GameLoop(); /* fake-08's own loop; never returns */
}
