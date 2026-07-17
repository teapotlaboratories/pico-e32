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
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

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

#ifdef MEASURE_FPS
    /* Opt-in fps measurement (DEFS='-D MEASURE_FPS=1'). Run our own loop, paced to the target fps, and
     * time Step() (fake-08's _update/_draw in the VM) vs drawFrame() (our unpack → RGB565 → 2× → blit)
     * separately. drawFrame's cost is ~content-independent (it always processes all 128×128 px + one
     * 256×256 blit), so this draw number is representative even on Celeste's title; the Step number is
     * whatever the cart runs this frame. "max fps" = compute-bound ceiling = 1000 / (step+draw). Logs
     * every 30 frames over UART. drawMode is ignored by drawFrame, so pass 0. */
    ESP_LOGI(TAG, "MEASURE_FPS: timing Step vs drawFrame + heap; spike frames (step>30ms) logged with heap delta");
    {
        int64_t acc_step = 0, acc_draw = 0, worst = 0;
        int frames = 0;
        long total = 0;
        while (true) {
            uint32_t h0 = esp_get_free_heap_size();       /* total free before Step (Lua uses the sys heap) */
            int64_t t0 = esp_timer_get_time();
            vm->Step();
            int64_t t1 = esp_timer_get_time();
            host->drawFrame(vm->GetPicoInteralFb(), vm->GetScreenPaletteMap(), 0);
            int64_t t2 = esp_timer_get_time();
            uint32_t h1 = esp_get_free_heap_size();        /* after Step: net Lua alloc/free this frame */
            int64_t step_us = t1 - t0, comp = t2 - t0;
            /* isolate spike frames: is the heavy Step allocating (room-load spawn) or stable (pure compute)? */
            if (step_us > 30000) {
                ESP_LOGW(TAG, "SPIKE f%ld: step=%.1f ms | heap %u->%u (%+d B) | low-water %u | int-free %u",
                         total, step_us / 1000.0, (unsigned)h0, (unsigned)h1, (int)(h1 - h0),
                         (unsigned)esp_get_minimum_free_heap_size(),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            }
            acc_step += step_us;
            acc_draw += (t2 - t1);
            if (comp > worst) worst = comp;
            frames++;
            total++;
            if (frames >= 30) {
                double s = acc_step / 1000.0 / frames, d = acc_draw / 1000.0 / frames, c = s + d;
                ESP_LOGI(TAG, "f%ld: step=%.2f draw=%.2f ms | max %.1f fps | worst %.2f ms | heap-free %u",
                         total, s, d, 1000.0 / c, worst / 1000.0, (unsigned)esp_get_free_heap_size());
                acc_step = acc_draw = 0;
                frames = 0;
                worst = 0;
            }
            host->waitForTargetFps();
        }
    }
#else
    ESP_LOGI(TAG, "entering GameLoop");
    vm->GameLoop(); /* fake-08's own loop; never returns */
#endif
}
