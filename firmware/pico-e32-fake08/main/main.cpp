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

#include "sdcard_spi.h"   /* the board-agnostic SD component (components/sdcard_spi) */

#include <vector>
#include <string>

static const char *TAG = "fake08";

/* App-owned cart-storage policy. SD_MOUNT_POINT is both where the SD mounts AND where ESP32Host
 * scans for carts (host->setCartDirectory). The board owns the SD *wiring* (board_sd_config); the
 * app owns the *policy* (mount point, no-format, flash fallback). A board with no SD slot never
 * defines BOARD_HAS_SD, so the whole mount path below compiles out. */
#define SD_MOUNT_POINT "/sdcard"
#ifndef BOARD_HAS_SD
#define BOARD_HAS_SD 0
#endif

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
/* A minimal PICO-8 cart (ours): a square you move with the d-pad that recolours on O/X, plus a corner
 * marker and a frame counter. The Cart ctor detects the "pico" magic and parses this as .p8 text
 * (loadCartFromString). Exercises cls / rectfill / print / btn and both _update and _draw — and makes
 * the input backend visible on the panel. No __gfx__ needed — print() uses fake-08's built-in font. */
static const char TEST_CART[] =
    "pico-8 cartridge version 41\n"
    "__lua__\n"
    "x=60 y=60 t=0\n"
    "function _update()\n"
    " t+=1\n"
    " if btn(0) then x-=2 end\n"
    " if btn(1) then x+=2 end\n"
    " if btn(2) then y-=2 end\n"
    " if btn(3) then y+=2 end\n"
    " x=mid(0,x,120)\n"
    " y=mid(0,y,120)\n"
    "end\n"
    "function _draw()\n"
    " cls(1)\n"
    " rectfill(0,0,7,7,8)\n"
    " local c=12\n"
    " if btn(4) then c=11 end\n"
    " if btn(5) then c=14 end\n"
    " rectfill(x,y,x+7,y+7,c)\n"
    " print(\"input test\",4,2,7)\n"
    " print(\"frame \"..t,4,120,7)\n"
    "end\n";
#endif

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "fake-08 port: draw-only milestone booting");

    if (board_lcd_init() != ESP_OK) {
        ESP_LOGE(TAG, "board_lcd_init failed");
        return;
    }

    /* Mount the SD for carts. The board owns the wiring (board_sd_config); the app owns the policy
     * (mount point, no-format). Non-fatal: no card / no SD slot / mount failure -> the flash cart. */
    esp_err_t sd_ret = ESP_ERR_NOT_FOUND;      /* "no SD this boot" until a mount proves otherwise */
#if BOARD_HAS_SD
    sdcard_spi_config_t sdcfg;
    sdcard_spi_config_default(&sdcfg);          /* policy defaults: no-format, 20 MHz, max_files */
    sdcfg.mount_point = SD_MOUNT_POINT;          /* app policy; board_sd_config leaves it untouched */
    if (board_sd_config(&sdcfg)) {               /* board fills host / pins / owns_bus */
        sd_ret = sdcard_spi_mount(&sdcfg);
    }
#endif

    /* fake-08 boot sequence (mirrors source/main.cpp:39-51). */
    Host    *host   = new Host(0, 0);
    PicoRam *memory = new PicoRam();
    memory->Reset();
    Audio   *audio  = new Audio(memory);
    Logger_Initialize(host->logFilePrefix());
    Vm      *vm     = new Vm(host, memory, nullptr, nullptr, audio);

    host->setUpPaletteColors();  /* must precede oneTimeSetup — it builds the RGB565 LUT */
    host->oneTimeSetup(audio);
    /* Resume fake-08's game-loop coroutine at 60 Hz (matches upstream source/main.cpp). The coroutine
     * self-divides: a _update60 cart runs 60 fps, a 30 fps cart (_update, e.g. Celeste) burns an extra
     * yield() and runs one frame per two resumes = 30 fps. Pacing at 30 Hz ran 30 fps carts at HALF
     * speed (15 fps of motion). The host resumes at 60; the cart's own loop sets its logical rate. */
    host->setTargetFps(60);

    /* Cart source ladder: an SD cart if a card is mounted and holds a .p8/.p8.png, else the flash cart. */
    host->setCartDirectory(SD_MOUNT_POINT);
    std::vector<std::string> carts = host->listcarts();
    vm->SetCartList(carts);

    bool loaded = false;
#ifndef FORCE_FLASH_CART
    if (sd_ret == ESP_OK && !carts.empty()) {
        ESP_LOGI(TAG, "loading SD cart: %s", carts[0].c_str());
        loaded = vm->LoadCart(carts[0], false); /* std::string overload -> reads the file over VFS */
        if (!loaded) ESP_LOGW(TAG, "SD cart failed to load; falling back to the flash cart");
    }
#else
    ESP_LOGW(TAG, "FORCE_FLASH_CART: ignoring any SD cart, running the flash cart"); /* dev/demo only */
    (void)sd_ret;
#endif
    if (!loaded) {
        ESP_LOGI(TAG, "loading %s (%u bytes)", CART_NAME, (unsigned)CART_LEN);
        loaded = vm->LoadCart(CART_BYTES, CART_LEN, false);
    }
    if (!loaded) {
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
#elif defined(TELEMETRY)
    /* Opt-in serial telemetry (DEFS='-D TELEMETRY=1') for hardware-in-the-loop input solving/verification.
     * Mirrors GameLoop() but, each frame, prints the Celeste player position + current room over UART via
     * printh. It reads them with the public Vm::ExecuteLua, which runs in the cart's sandbox environment
     * (where Celeste's `objects`/`player`/`room` globals live) — so this needs NO cart edit and NO change to
     * the vendored fake-08 source. One line per Step (60 Hz resume; a 30 fps cart updates every 2nd line):
     *   "T <x> <y> <room.x> <room.y> <spd.x> <spd.y> <djump>"   ('x' fields until the player object exists).
     * Pairs with INPUT_BACKEND=serial so tools/celeste_playtest.py can frame-sync input and detect the
     * room transition (room.x/y change) that proves a level was cleared. See docs/runtime. */
    ESP_LOGI(TAG, "TELEMETRY: per-frame player pos over UART (T <frame> x y rx ry sx sy dj)");
    {
        while (true) {
            host->waitForTargetFps();
            vm->Step();
            host->drawFrame(vm->GetPicoInteralFb(), vm->GetScreenPaletteMap(), 0);
            /* Inject the (monotonic, per-Step) frame counter into the printh line so the driver has an
             * exact frame clock to sync input to. One line per Step (60 Hz resume; 30 fps cart updates
             * every 2nd line). ExecuteLua runs in the cart sandbox where objects/player/room live. */
            int fc = vm->GetFrameCount();
            char snip[420];
            snprintf(snip, sizeof(snip),
                "local p for o in all(objects) do if o.type==player then p=o end end "
                "if p then printh('T %d '..p.x..' '..p.y..' '..room.x..' '..room.y..' '..p.spd.x..' '..p.spd.y..' '..p.djump) "
                "else printh('T %d x x '..room.x..' '..room.y..' x x x') end", fc, fc);
            vm->ExecuteLua(snip, "");
        }
    }
#else
    ESP_LOGI(TAG, "entering GameLoop");
    vm->GameLoop(); /* fake-08's own loop; never returns */
#endif
}
