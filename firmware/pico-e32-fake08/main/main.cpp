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
#if defined(LAUNCHER) || defined(FB_DUMP)
#include "carousel_launcher.h"   /* native cover-art carousel + the shared compressed FB_DUMP helper */
#endif
#include "vm.h"
#include "PicoRam.h"
#include "Audio.h"
#include "logger.h"

#include "sdcard_spi.h"   /* the board-agnostic SD component (components/sdcard_spi) */
#include "driver/uart.h"  /* TELEMETRY_HOST_CFG (dev/HITL only): read a startup telemetry-tail command from the console */
#if defined(TELEMETRY_HOST_CFG) && CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"  /* P4 board: the console is native USB-Serial-JTAG, not UART0 */
#endif
#include "input.h"        /* input_set_frame — hand the fc-scheduled input backend the frame clock (dev/HITL) */

#include <vector>
#include <string>

static const char *TAG = "fake08";

#ifdef SHOW_FPS
/* On-screen FPS HUD (file scope — an extern "C" linkage-spec is illegal inside a function). The TELEMETRY
 * loop takes over the HUD (g_hud_owned_by_app) to draw the cart's true achieved game-frame fps; board.cpp
 * defines the flag + the draw, and ESP32Host's generic render-loop meter stands down when the flag is set. */
extern "C" void board_lcd_draw_fps(int fps);
extern "C" volatile int g_hud_owned_by_app;
#endif

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
#elif defined(RACER) && __has_include("pico_racer_p8.h")
/* `-D RACER=1` embeds Pico Racer (the gitignored assets/pico_racer_p8.h, a .p8.png — the Cart ctor detects
 * the PNG magic and decodes it) as the flash cart, mirroring the CELESTE build. Third-party; never committed. */
#include "pico_racer_p8.h"
#define CART_BYTES RACER_P8
#define CART_LEN   RACER_P8_LEN
#define CART_NAME  "Pico Racer.p8.png"
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

#if defined(TELEMETRY_HOST_CFG) && !defined(MEASURE_FPS)
/* Host-configurable telemetry (dev/HITL). The generic `T <fc> <step_us> <draw_us> ` prefix is fixed; the cart
 * appends a TAIL (a Lua expression returning its state fields, space-separated) sent at startup — so the
 * firmware carries NO per-cart telemetry and a new cart is drop-in. This fallback tail (Celeste's) is used
 * only if a HOST_CFG build gets no tail in time. NOTE: a plain (non-HOST_CFG) TELEMETRY build keeps the
 * ORIGINAL inline Celeste telemetry below — no per-frame closure — so Celeste's frame-precise timing is
 * unchanged. The tail runs in the cart sandbox via ExecuteLua. */
static const char TELEMETRY_DEFAULT_TAIL[] =
    "(function() local p for o in all(objects) do if o.type==player then p=o end end "
    "if p then return p.x..' '..p.y..' '..room.x..' '..room.y..' '..p.spd.x..' '..p.spd.y..' '..p.djump "
    "else return 'x x '..room.x..' '..room.y..' x x x' end end)()";
#endif

#if defined(GC_MANUAL)
/* ── Manual / core-1-offloaded Lua GC (perf experiment, `-D GC_MANUAL=1` [+ `-D GC_CORE1=1`]) ─────────────
 * The Pico Racer cart churns ~5-6 KB of Lua tables per frame; z8lua's automatic collector answers with
 * incremental work on the allocating frames plus periodic stop-the-world collections (measured Step spikes to
 * ~65 ms). Here the host disables the auto collector and drives it in a BOUNDED time slice each frame, so one
 * big collection spreads over several frames — the peak drops. With GC_CORE1 that slice runs on core 1 WHILE
 * core 0 blits: drawFrame touches no Lua and z8lua's GC is non-moving (mark-sweep), so the framebuffer it
 * reads stays put — the collection is hidden behind the ~6 ms draw. Barrier before the next Step (which
 * mutates the heap). Auto-GC is re-enabled only if the live set climbs past a guard (OOM safety). */
#include "freertos/semphr.h"
#ifndef GC_BUDGET_US
#define GC_BUDGET_US 6000            /* per-frame GC time slice */
#endif
#ifndef GC_STEP_KB
#define GC_STEP_KB   4               /* lua_gc(GCSTEP) work unit per call */
#endif
#ifndef GC_GUARD_KB
#define GC_GUARD_KB  1024            /* if live Lua memory exceeds this, catch up hard (avoid runaway growth) */
#endif
static Vm *s_gc_vm = nullptr;
static inline int gc_run_slice(int budget_us) {   /* step until a cycle completes or the budget is spent */
    int64_t g0 = esp_timer_get_time();
    while ((int)(esp_timer_get_time() - g0) < budget_us) { if (s_gc_vm->GcStep(GC_STEP_KB)) break; }
    if (s_gc_vm->GcCountKb() > GC_GUARD_KB) while (!s_gc_vm->GcStep(64)) { }   /* guard: finish a cycle now */
    return (int)(esp_timer_get_time() - g0);
}
#ifdef GC_CORE1
static SemaphoreHandle_t s_gc_go, s_gc_done;
static volatile int s_gc_us_out;
static void gc_task(void *) {
    for (;;) { xSemaphoreTake(s_gc_go, portMAX_DELAY); s_gc_us_out = gc_run_slice(GC_BUDGET_US); xSemaphoreGive(s_gc_done); }
}
#endif
#endif

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "fake-08 port: draw-only milestone booting");

    if (board_lcd_init() != ESP_OK) {
        ESP_LOGE(TAG, "board_lcd_init failed");
        return;
    }


#ifdef LCD_FILL
    /* Bench utility (NOT a game build): flood the panel one solid colour, then idle forever. Used to
     * turn an idle board into a fill light for the bench camera. -D LCD_FILL=1 = white; or pass a
     * 0xRRGGBB value, e.g. -D LCD_FILL=0xffffff. No SD / VM / host bring-up happens past this point. */
    {
        unsigned rgb = ((LCD_FILL) == 1) ? 0xffffffu : (unsigned)(LCD_FILL);
        board_lcd_fill(board_lcd_rgb565((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff));
        ESP_LOGI(TAG, "LCD_FILL: solid 0x%06x, idling as a fill light", rgb);
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif

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
#elif BOARD_HAS_SDMMC
    /* Native SDMMC boards (the P4) own the whole mount behind board_sd_mount — SPI's config/mount seam doesn't
     * apply. Same non-fatal contract: non-ESP_OK just means "run the flash cart". The launcher hands this host
     * over to the radio for the duration of a WiFi session (board_sd_unmount) and remounts after — see WC-6. */
    sd_ret = board_sd_mount(SD_MOUNT_POINT);
#endif

    /* fake-08 boot sequence (mirrors source/main.cpp:39-51). Pass the board's panel size so the host
     * centres the game blit for this geometry (S3 320x480, P4 480x800). */
    Host    *host   = new Host(BOARD_LCD_H_RES, BOARD_LCD_V_RES);
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
#ifdef LAUNCHER
    /* Launcher build (`-D LAUNCHER=1`): if the SD mounted, run the native cover-art carousel over the card
     * (decodes each .p8.png's label art, 3-up carousel, deck-driven folder navigation). It returns the cart
     * the user chose; we load that. If the user exits without choosing (or it errors), fall back to fake-08's
     * built-in text browser, then the flash cart. */
    if (sd_ret == ESP_OK) {
        std::string pick = carousel_launcher_run(host, SD_MOUNT_POINT);
        if (!pick.empty()) {
            ESP_LOGI(TAG, "LAUNCHER: loading %s", pick.c_str());
            loaded = vm->LoadCart(pick, false);
            if (!loaded) ESP_LOGW(TAG, "chosen cart failed to load; falling back");
        }
        if (!loaded) loaded = vm->LoadBiosCart();   /* exited/failed -> the text browser */
    }
#else
    if (sd_ret == ESP_OK && !carts.empty()) {
        ESP_LOGI(TAG, "loading SD cart: %s", carts[0].c_str());
        loaded = vm->LoadCart(carts[0], false); /* std::string overload -> reads the file over VFS */
        if (!loaded) ESP_LOGW(TAG, "SD cart failed to load; falling back to the flash cart");
    }
#endif
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

#if defined(LUABENCH)
    /* Dev micro-benchmark (`-D LUABENCH=1`): a deterministic, table-and-global-heavy Lua loop timed in C, to
     * isolate VM changes (e.g. the OP_GETTABLE/GETTABUP fast-get) from noisy game measurements. Runs in the
     * cart sandbox (so field reads + PICO-8 API globals resolve exactly as in a cart), reports us over serial,
     * then halts. Not compiled into any normal build. */
    {
        for (int i = 0; i < 60; i++) vm->Step();   /* let the cart coroutine build __cart_sandbox + the API */
        /* Nested loops (each count <= 32767, fix32's integer range) => ~90k iterations, each doing 8 field
         * reads (OP_GETTABLE) + 4 global calls (OP_GETTABUP) — exactly what fast-get targets. */
        const char *bench =
            "local t={a=1,b=2,c=3,d=4,e=5,f=6,g=7,h=8} local s=0 local n=0\n"
            "for j=1,300 do for i=1,300 do n=n+1\n"
            "  s = s + t.a+t.b+t.c+t.d+t.e+t.f+t.g+t.h + abs(t.a)+flr(t.b)+abs(t.c)+flr(t.d)\n"
            "end end printh('BENCH n='..n..' s='..s) return s";
        for (int rep = 0; rep < 5; rep++) {
            int64_t b0 = esp_timer_get_time();
            vm->ExecuteLua(bench, "");
            int64_t b1 = esp_timer_get_time();
            printf("LUABENCH: %lld us\n", (long long)(b1 - b0));
            fflush(stdout);
        }
        printf("LUABENCH: done\n"); fflush(stdout);
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif

#if defined(TELEMETRY_HOST_CFG) && !defined(MEASURE_FPS)
    /* Dev/HITL only — `-D TELEMETRY_HOST_CFG=1` (never in production; TELEMETRY itself is dev-only). Let the
     * host set the telemetry TAIL at startup so the firmware carries no per-cart state: install UART0 RX
     * (idempotent with the serial input backend, which installs it lazily on the first scanInput), prompt
     * "CFG?", and read one `TT<lua-expr>` line BEFORE the game loop consumes any input. No answer in the
     * window -> keep the default tail. This is the whole "backend serial command"; it compiles out otherwise. */
    const char *tele_tail = TELEMETRY_DEFAULT_TAIL;
    static char tele_tail_buf[1280];   /* a rich tail (curve + object-queue reads) can run ~750 chars */
    {
        /* Read the host's TT<expr> from the SAME console the host is wired to — UART0 on the S3 (CP2104
         * bridge), native USB-Serial-JTAG on the P4. Reading UART0 on a USB-JTAG board gets nothing, so the
         * board would silently fall back to the (Celeste) default tail — wrong for any other cart. Mirror the
         * serial input backend (components/input/input_serial.c), which branches on the same console macro. */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
        /* RX ring must hold the WHOLE TT<tail> line: the host writes it in one fast USB burst (unlike the S3's
         * UART, where the bytes trickle in at line rate and a small ring drains fine), so a 256-byte ring would
         * overflow and silently truncate the tail -> the __tele def fails to parse. Size it well past the tail. */
        usb_serial_jtag_driver_config_t ujcfg = { .tx_buffer_size = 256, .rx_buffer_size = 2048 };
        usb_serial_jtag_driver_install(&ujcfg);   /* idempotent: already-installed returns INVALID_STATE */
#else
        if (!uart_is_driver_installed(UART_NUM_0)) uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
#endif
        printf("CFG?\n"); fflush(stdout);
        char line[1280]; int li = 0; int64_t tcfg0 = esp_timer_get_time();
        while (esp_timer_get_time() - tcfg0 < 4000000) {          /* 4s window for the host to answer */
            uint8_t c;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
            if (usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(50)) != 1) continue;
#else
            if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(50)) != 1) continue;
#endif
            if (c == '\n' || c == '\r') { if (li > 0) break; else continue; }
            if (li < (int)sizeof(line) - 1) line[li++] = (char)c;
        }
        line[li] = 0;
        if (li >= 2 && line[0] == 'T' && line[1] == 'T') {         /* `TT<expr>` -> the cart's tail */
            snprintf(tele_tail_buf, sizeof(tele_tail_buf), "%s", line + 2);
            tele_tail = tele_tail_buf;
            ESP_LOGI(TAG, "TELEMETRY: host-set tail (%d chars)", li - 2);
        } else {
            ESP_LOGW(TAG, "TELEMETRY: no host tail within 4s; using the fallback (Celeste)");
        }
    }
#if defined(TELEMETRY_BAUD) && !CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    /* Bump the console UART to a higher baud for the run (the CFG handshake above stays at the safe default).
     * Announce it, let the line flush, then switch; the host reads "BAUD <n>" and switches to match. Halving
     * the per-frame serialization time (and its jitter) tightens the closed-loop control latency. UART-only:
     * a USB-Serial-JTAG console (the P4) has no wire baud, so this is compiled out there — no bogus announce. */
    printf("BAUD %d\n", TELEMETRY_BAUD); fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(40));
    uart_set_baudrate(UART_NUM_0, TELEMETRY_BAUD);
#endif
#endif

#ifdef RND_SEED
    /* Pin PICO-8's PRNG so a cart whose state depends on rnd() (e.g. Pico Racer's traffic/obstacle spawns)
     * replays frame-for-frame against a host-built trace. Match the sim's CANONICAL setup exactly
     * (test/playtest/fake08-sim/sim.cpp: sim_init runs 60 boot Steps, then the host calls Lua srand): run the
     * same 60 boot Steps, then `srand(RND_SEED)`. srand is a PICO-8 API (registered in vm.cpp), so this
     * ExecuteLua seeds the identical rngState the host seeded — the device's rnd() sequence then matches. */
    for (int i = 0; i < 60; i++) vm->Step();
    { char s[24]; snprintf(s, sizeof(s), "srand(%d)", RND_SEED); vm->ExecuteLua(s, ""); }
    ESP_LOGI(TAG, "RND_SEED=%d: PRNG pinned after 60 boot steps (deterministic rnd for trace replay)", RND_SEED);
#endif

#ifdef CELESTE_START
    /* Skip the title for the fc-scheduled HITL play-test: begin_game() + load_room(0,0), mirroring the sim's
     * sim_start_room (test/playtest/fake08-sim/sim.cpp). The telemetry loop's Steps then run the spawn
     * animation, so the player appears at room (0,0)'s spawn (8,96) with no title press — the scheduled input
     * backend takes only fc-tagged commands, not the raw title-start key. Dev/HITL only. */
    vm->ExecuteLua("begin_game() load_room(0,0)", "");
    ESP_LOGI(TAG, "CELESTE_START: begin_game + load_room(0,0) (skip title for fc-scheduled HITL)");
#endif

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
    /* Opt-in serial telemetry (DEFS='-D TELEMETRY=1') for hardware-in-the-loop input solving/verification
     * AND on-the-fly fps measurement (this unifies the old MEASURE_FPS timing into the telemetry stream, so a
     * play-test measures its own fps). Mirrors GameLoop() and, each Step, prints over UART via printh:
     *   "T <frame> <step_us> <draw_us> <x> <y> <room.x> <room.y> <spd.x> <spd.y> <djump>"
     * The `<frame> <step_us> <draw_us>` prefix is GENERIC (frame clock + this Step's compute time); the player
     * tail is Celeste-read via the public Vm::ExecuteLua (cart sandbox — NO cart edit, NO vendored change;
     * 'x' fields until the player exists). step_us times vm->Step() (the cart's _update/_draw), draw_us times
     * host->drawFrame() (unpack→RGB565→2×→blit); the ExecuteLua telemetry poke is deliberately NOT timed, so
     * the numbers are the cart's real per-frame compute. The driver frame-syncs input to <frame>, detects the
     * room transition (room.x/y change = a clear), and aggregates <step_us>/<draw_us> into achieved/headroom
     * fps. One line per Step (60 Hz resume; a 30 fps cart updates every 2nd line). See docs/runtime. */
    /* The TAIL is cart-specific but NOT baked in here: `tele_tail` is the default (Celeste) or the host-set
     * expression read at startup (TELEMETRY_HOST_CFG). Only the generic prefix + fps aggregation live here. */
    ESP_LOGI(TAG, "TELEMETRY: per-Step 'T <fc> <step_us> <draw_us> <tail>' over UART (tail: host-set or default)");
    {
        /* fc-scheduled deadline miss-rate readout: the scheduled backend counts fed/miss/applied on the board
         * (input_sched_stats); stream it as a periodic `TS <fed> <miss> <applied>` line (~1x/s) so the host can
         * read the real on-device miss rate per lead k — the last quantitative number the design turns on. Only
         * the scheduled backend tracks it (others report zeros, so this compiles + runs for any backend);
         * computed once here so the per-frame path stays cheap. It rides the same UART as telemetry but uses a
         * distinct `TS` prefix, so the host's `T` line parser skips it and the binary reader's 0xAA sync-scan
         * skips it too (an ASCII line never contains 0xAA). */
        const bool sched_backend = (strcmp(input_backend_name(), "scheduled") == 0);
        int stat_steps = 0;
#ifdef TELEMETRY_HOST_CFG
        bool tele_defined = false;   /* precompile the telemetry line into a Lua function once (see below) */
#endif
#ifdef SHOW_FPS
        /* Own the on-screen HUD and show the cart's ACHIEVED game-frame fps — the SAME figure the host fps
         * meter reports (harness.py FpsMeter): a 30 fps game-frame's compute is step_us+draw_us summed over its
         * 2 resumes, achieved = min(target 30, 1e6/compute), displayed as a rolling mean. This is the cart's own
         * compute-bound rate (dev telemetry/pacing excluded) and is bounded by the 30 target — unlike the raw
         * resume rate the generic ESP32Host meter would show (~2x higher). That meter stands down via the flag. */
        g_hud_owned_by_app = 1;
        int hud_steps = 0; int64_t hud_compute = 0;       /* accumulate 2 resumes -> one game-frame's compute */
        double hud_ach_sum = 0; int hud_ach_n = 0;         /* rolling mean of achieved fps over the display window */
        int64_t hud_t0 = esp_timer_get_time();
#endif
#if defined(GC_GEN)
        /* Generational GC (`-D GC_GEN=1`): keep the AUTOMATIC collector but switch it to generational mode —
         * frequent CHEAP minor collections of young garbage, rare (costly) major ones. A good fit for a cart
         * that churns short-lived objects (the racer's traffic spawns + dies each frame), which the incremental
         * collector pays for with a big atomic mark. GC_MAJORINC raises the heap-growth gate before a major. */
        vm->GcSetGenerational();
#ifdef GC_MAJORINC
        vm->GcSetMajorInc(GC_MAJORINC);
#endif
        ESP_LOGI(TAG, "GC: generational mode (automatic)");
#endif
#if defined(GC_MANUAL)
        s_gc_vm = vm;
        vm->GcSetAuto(false);          /* drive the collector manually in bounded slices (see block above) */
#ifdef GC_CORE1
        s_gc_go = xSemaphoreCreateBinary(); s_gc_done = xSemaphoreCreateBinary();
        xTaskCreatePinnedToCore(gc_task, "gc", 4096, nullptr, 5, nullptr, 1);
        ESP_LOGI(TAG, "GC: manual bounded stepping on CORE 1, overlapped with the blit (budget %d us)", GC_BUDGET_US);
#else
        ESP_LOGI(TAG, "GC: manual bounded stepping on core 0, sequential (budget %d us)", GC_BUDGET_US);
#endif
#endif
        while (true) {
            host->waitForTargetFps();
            /* fc-scheduled input (INPUT_BACKEND=scheduled): hand the backend the fc THIS Step will emit.
             * vm->Step() advances GetFrameCount() by ONE per Step (a 30 fps cart = 2 Steps/game-frame), and
             * telemetry emits GetFrameCount() right after Step, so the fc this iteration emits == before+1.
             * The backend applies fc-tagged commands when its clock reaches that fc. A no-op for every other
             * backend. Kept out of the t0..t1 window so step_us stays the cart's. */
            input_set_frame((uint32_t)vm->GetFrameCount() + 1u);
#ifdef SPIKE_HEAP_LOG
            uint32_t h0_spike = esp_get_free_heap_size();   /* dev diag: heap free right before this Step */
#endif
            int64_t t0 = esp_timer_get_time();
            vm->Step();
            int64_t t1 = esp_timer_get_time();
            int gc_us = 0; (void)gc_us;
#if defined(GC_MANUAL) && defined(GC_CORE1)
            s_gc_us_out = 0;
            xSemaphoreGive(s_gc_go);                                             /* core 1: GC this frame's slice */
            host->drawFrame(vm->GetPicoInteralFb(), vm->GetScreenPaletteMap(), 0); /* core 0: blit (no Lua) */
            xSemaphoreTake(s_gc_done, portMAX_DELAY);                            /* barrier before the next Step */
            gc_us = s_gc_us_out;
#elif defined(GC_MANUAL)
            gc_us = gc_run_slice(GC_BUDGET_US);                                  /* sequential on core 0 */
            host->drawFrame(vm->GetPicoInteralFb(), vm->GetScreenPaletteMap(), 0);
#else
            host->drawFrame(vm->GetPicoInteralFb(), vm->GetScreenPaletteMap(), 0);
#endif
            int64_t t2 = esp_timer_get_time();
            int fc = vm->GetFrameCount();
            int step_us = (int)(t1 - t0), draw_us = (int)(t2 - t1);   /* draw_us = post-step wall time (incl. GC/overlap) */
#ifdef SPIKE_HEAP_LOG
            /* Dev diag (`-D SPIKE_HEAP_LOG=1`): on a slow frame, print the step/GC/post-step split + heap free
             * before->after. Baseline (auto GC): a spike shows in `step` with heap JUMPING UP (a collector pause);
             * with GC_MANUAL the collection moves into `gc` (and, with GC_CORE1, hides inside `post`). Interleaves
             * as text in the telemetry stream; the host's binary reader skips it. `lua` = live Lua memory. */
            if (step_us + draw_us > 20000) {
                uint32_t h1_spike = esp_get_free_heap_size();
                ESP_LOGW(TAG, "SPIKE fc%d step=%d gc=%d post=%d | heap %u->%u (%+d B) | int-free %u | lua %d KB",
                         fc, step_us, gc_us, draw_us, (unsigned)h0_spike, (unsigned)h1_spike,
                         (int)(h1_spike - h0_spike), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         vm->GcCountKb());
            }
#endif
            char snip[1024]; (void)snip;
#ifdef SHOW_FPS
            hud_compute += step_us + draw_us;
            if (++hud_steps >= 2) {                        /* one 30 fps game-frame = 2 resumes */
                double comp = (double)hud_compute;
                double ach = comp > 0 ? 1e6 / comp : 30.0;
                if (ach > 30.0) ach = 30.0;                /* achieved is capped at the target, like the meter */
                hud_ach_sum += ach; hud_ach_n++;
                hud_steps = 0; hud_compute = 0;
                if (t2 - hud_t0 >= 400000 && hud_ach_n > 0) {   /* repaint ~2.5x/s with the window's mean */
                    board_lcd_draw_fps((int)(hud_ach_sum / hud_ach_n + 0.5));
                    hud_ach_sum = 0; hud_ach_n = 0; hud_t0 = t2;
                }
            }
#endif
#if defined(TELEMETRY_HOST_CFG) && defined(TELEMETRY_BINARY)
            /* BINARY telemetry (default for the closed-loop path): fewer bytes -> less serialization time AND
             * jitter, which tightens the control latency. The host-set tail is a POKE statement (compiled once)
             * that writes the cart's fields as fixed-point int16s into scratch RAM 0x4300..; the firmware ships
             * them raw, sync-framed:  0xAA 0x55  fc(int32)  step_us(u16)  draw_us(u16)  <BYTES from 0x4300>.
             * No fix32->string formatting or GC, and it lands the device's numbers at the sim reader's exact
             * scaling. */
            if (!tele_defined) {
                char def[1408];
                snprintf(def, sizeof(def), "function __tele() %s end", tele_tail);
                vm->ExecuteLua(def, ""); tele_defined = true;
            }
            vm->ExecuteLua("__tele()", "");
            {
                uint8_t pkt[10 + TELEMETRY_BINARY_BYTES];
                pkt[0] = 0xAA; pkt[1] = 0x55;
                memcpy(pkt + 2, &fc, 4);                       /* int32 LE (ESP32 is little-endian) */
                uint16_t su = (uint16_t)step_us, du = (uint16_t)draw_us;
                memcpy(pkt + 6, &su, 2); memcpy(pkt + 8, &du, 2);
                memcpy(pkt + 10, memory->data + 0x4300, TELEMETRY_BINARY_BYTES);
                /* Ship on the SAME console the host reads — USB-Serial-JTAG on the P4, UART0 on the S3.
                 * Writing UART0 on a USB-JTAG board goes to the (unconnected) UART pins and errors. */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
                usb_serial_jtag_write_bytes((const char *)pkt, sizeof(pkt), portMAX_DELAY);
#else
                uart_write_bytes(UART_NUM_0, (const char *)pkt, sizeof(pkt));
#endif
            }
#elif defined(TELEMETRY_HOST_CFG)
            /* Cart-agnostic + FAST (text): compile the telemetry line into a Lua function ONCE (its object-queue
             * scan / curve loop parsed a single time), then each frame ExecuteLua only re-parses a tiny
             * `__tele(fc,step,draw)` call — dropping the per-frame parse that dragged fps. */
            if (!tele_defined) {
                char def[1408];
                snprintf(def, sizeof(def),
                    "function __tele(f,s,d) printh('T '..f..' '..s..' '..d..' '..(%s)) end", tele_tail);
                vm->ExecuteLua(def, "");
                tele_defined = true;
            }
            snprintf(snip, sizeof(snip), "__tele(%d,%d,%d)", fc, step_us, draw_us);
            vm->ExecuteLua(snip, "");
#else
            /* plain TELEMETRY build: the ORIGINAL inline Celeste telemetry — no per-frame closure, so the
             * frame-precise timing Celeste's play-test depends on is unchanged. */
            snprintf(snip, sizeof(snip),
                "local p for o in all(objects) do if o.type==player then p=o end end "
                "if p then printh('T %d %d %d '..p.x..' '..p.y..' '..room.x..' '..room.y..' '..p.spd.x..' '..p.spd.y..' '..p.djump) "
                "else printh('T %d %d %d x x '..room.x..' '..room.y..' x x x') end",
                fc, step_us, draw_us, fc, step_us, draw_us);
            vm->ExecuteLua(snip, "");
#endif
            /* Stream the fc-scheduled deadline miss-rate ~once/second (60 Steps = ~30 game-frames at 2 Steps/
             * frame). Outside the step_us/draw_us timing window, and scheduled-backend-only, so it perturbs
             * neither the fps numbers nor other builds. The host reads the latest `TS` line before it stops. */
            if (sched_backend && ++stat_steps >= 60) {
                stat_steps = 0;
                uint32_t sfed = 0, smiss = 0, sapplied = 0;
                input_sched_stats(&sfed, &smiss, &sapplied);
                printf("TS %u %u %u\n", (unsigned)sfed, (unsigned)smiss, (unsigned)sapplied);
                fflush(stdout);
            }
        }
    }
#elif defined(FB_DUMP)
    /* Dev "screenshot over serial": run the loop so the game + touch deck composite into the panel
     * framebuffer, then stream the live DPI framebuffer (RGB565) over the console once, framed, for a
     * camera-free host-side PNG (tools/fb_screenshot.py). P4-only (uses board_lcd_framebuffer). */
    ESP_LOGI(TAG, "FB_DUMP: streaming the framebuffer after warmup");
    {
        const int target = 120;   /* ~2 s at 60 Hz resume: deck drawn (frame-0 scanInput) + game settled */
        const int period = 90;    /* then re-dump every ~1.5 s so a headless driver can catch gameplay frames */
        for (int f = 0;; f++) {
            vm->Step();
            host->drawFrame(vm->GetPicoInteralFb(), vm->GetScreenPaletteMap(), 0);
            if (f >= target && (f - target) % period == 0) {
                carousel_fb_dump();   /* board-appropriate FB dump (compressed on P4 / raw on S3), shared helper */
            }
            host->waitForTargetFps();
        }
    }
#elif defined(GC_MANUAL)
    /* Shipped-style loop (touch / no telemetry) WITH the bounded GC — a faithful copy of Vm::GameLoop() (which
     * stays byte-identical to upstream, so we don't edit it) plus the collector driven in a bounded slice each
     * frame; with GC_CORE1 the slice runs on core 1 overlapped with the Lua-free blit. The on-screen fps HUD is
     * the board's generic meter (g_hud_owned_by_app stays 0 here). See the GC block above app_main. */
    s_gc_vm = vm;
    vm->GcSetAuto(false);
#ifdef GC_CORE1
    s_gc_go = xSemaphoreCreateBinary(); s_gc_done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(gc_task, "gc", 4096, nullptr, 5, nullptr, 1);
    ESP_LOGI(TAG, "entering GameLoop (bounded GC on core 1, budget %d us)", GC_BUDGET_US);
#else
    ESP_LOGI(TAG, "entering GameLoop (bounded GC on core 0, budget %d us)", GC_BUDGET_US);
#endif
    while (host->shouldRunMainLoop()) {
        host->waitForTargetFps();
        if (host->shouldQuit()) break;
        host->changeStretch();
        vm->Step();
#ifdef GC_CORE1
        xSemaphoreGive(s_gc_go);                                                 /* GC on core 1 ... */
        host->drawFrame(vm->GetPicoInteralFb(), vm->GetScreenPaletteMap(), 0);   /* ... while core 0 blits */
        xSemaphoreTake(s_gc_done, portMAX_DELAY);
#else
        gc_run_slice(GC_BUDGET_US);
        host->drawFrame(vm->GetPicoInteralFb(), vm->GetScreenPaletteMap(), 0);
#endif
        if (host->shouldFillAudioBuff()) {
            vm->FillAudioBuffer(host->getAudioBufferPointer(), 0, host->getAudioBufferSize());
            host->playFilledAudioBuffer();
        }
    }
#else
    ESP_LOGI(TAG, "entering GameLoop");
    vm->GameLoop(); /* fake-08's own loop; never returns */
#endif
}
