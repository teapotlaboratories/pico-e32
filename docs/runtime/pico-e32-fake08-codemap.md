# fake-08 port — code map (ESP32-S3 / pico-e32)

Function-level map of the port's new code to its [fake-08](https://github.com/jtothebell/fake-08) origin,
per [`.ai/AGENTS.md` → Porting](../../.ai/AGENTS.md). The runtime is a **port of fake-08** — `source/` is
**byte-identical to upstream** (`teapotlaboratories/fake-08 @ pico-e32`, forked from `jtothebell/fake-08`
`master` `814991a`); everything below is *additive* (a `platform/esp32` binding + the app entry) or a
*build-level* adaptation. Nothing in `source/` is edited. Every cited line was grepped in both trees.

Related: the [port plan](pico-e32-fake08-port.md), the [vendor worklog](../worklog/2026-07-17-fake08-port-vendor.md).

## New files

| New file | Role |
|---|---|
| `components/fake08/platform/esp32/ESP32Host.cpp` | Implements fake-08's `Host` interface for the ESP32-S3 |
| `components/fake08/platform/esp32/fake08_board.h` | The `board_lcd_*` display contract (extern "C") |
| `components/fake08/CMakeLists.txt` | ESP-IDF component build (the fork's build wrapper) |
| `firmware/pico-e32-fake08/main/main.cpp` | `app_main` — the ESP-IDF entry replacing `source/main.cpp` |
| `firmware/pico-e32-fake08/{CMakeLists.txt, main/CMakeLists.txt, sdkconfig.defaults}` | IDF project glue |

## `ESP32Host.cpp` ↔ fake-08

fake-08's `Host` is one concrete class (`source/host.h:60`); each platform supplies its method definitions.
`ESP32Host.cpp` defines the **23** methods **not** provided by the shared `source/hostCommonFunctions.cpp`
(which owns the other 13: palette/settings/filesystem). Reference platform = `platform/bittboy` (a simple
framebuffer device that outputs `uint16` RGB565 with stubbed audio — the closest analog).

| `ESP32Host.cpp` method | Declared | Modelled on (upstream) | Notes |
|---|---|---|---|
| `drawFrame(picoFb, screenPaletteMap, drawMode)` | `host.h:119` | `platform/bittboy/source/BittBoyHost.cpp:390` (default branch `:520`) | Same `getPixelNibble` unpack (`nibblehelpers.cpp:29`) + `_mapped16BitColors[screenPaletteMap[c]&0x8f]` LUT. Ours: `board_lcd_rgb565` LUT, 2× scale, **90° CW rotate** (divergence D4), one `board_lcd_blit`. Only default `drawMode`. |
| `oneTimeSetup(Audio*)` | `host.h:103` | `BittBoyHost.cpp:196` (LUT build `:222`) | Build the RGB565 LUT from `_paletteColors` via `board_lcd_rgb565`; alloc the scaled fb; clear the panel. No SDL. |
| `setUpPaletteColors()` | — | `source/hostCommonFunctions.cpp:29` | **Reused as-is** (shared), fills `_paletteColors[144]` from `hostVmShared.h:11-44`. |
| `scanInput()` | `host.h:111` | `BittBoyHost.cpp:292` | **Stub**: returns `InputState_t{}` (all zero) — divergence D3. |
| `setTargetFps` / `waitForTargetFps` | `host.h:107,117` | `BittBoyHost.cpp:252,364` | `esp_timer` + `vTaskDelay` instead of `SDL_GetTicks`/`SDL_Delay` — divergence D2. |
| `shouldFillAudioBuff`/`getAudioBufferPointer`/`getAudioBufferSize`/`playFilledAudioBuffer` | `host.h:121-124` | `BittBoyHost.cpp:573-585` | **Stub** — identical to bittboy's already-stubbed audio. Divergence D3. |
| `shouldRunMainLoop`/`shouldQuit`/`changeStretch`/`forceStretch` | `host.h:109,112,114,115` | `BittBoyHost.cpp:588,360,256,283` | Loop always runs; no quit; stretch is a no-op (we own the 128→panel scale). |
| `listcarts`/`listdirs`/`getCartDirectory`/`logFilePrefix`/`customBiosLua` | `host.h:130,146,144,133,135` | `BittBoyHost.cpp:596,634,630,618,622` | Return empty/`""` (no filesystem on the draw-only path). |
| `oneTimeCleanup`/`deltaTMs`/`overrideLogFilePrefix`/`setPlatformParams` | `host.h:126,128,132,154` | (bittboy `:237`; others desktop-only) | Cleanup frees the fb; the rest are inert no-ops. |

## `main.cpp` (`app_main`) ↔ `source/main.cpp`

The ESP-IDF entry mirrors fake-08's desktop `source/main.cpp:39-99` boot sequence, then hands off to
fake-08's **own** `Vm::GameLoop()` (`source/vm.cpp:860`) — the frame loop is fake-08's, not ours.

| `app_main` step | `source/main.cpp` | Notes |
|---|---|---|
| `new Host / new PicoRam / memory->Reset() / new Audio / Logger_Initialize / new Vm` | `:39-46` | Identical order. `Vm` ctor builds `Graphics`/`Input` and boots the Lua VM (`vm.cpp:300` `luaL_newstate`). |
| `setUpPaletteColors → oneTimeSetup → setTargetFps → SetCartList` | `:49-64` | Identical order. |
| `vm->LoadCart(cartBytes, len, false)` | cf. `:87` (`LoadCart` at `vm.cpp:495`) | **Divergence D1**: flash-embedded `.p8` byte array (the `"pico"`-magic text path, `cart.cpp:398`) instead of a file. |
| `vm->vm_run()` | `:94` (`vm.cpp:1274`) | Starts the cart coroutine. |
| `vm->GameLoop()` | `:99` | Reused unchanged. |
| `board_lcd_init()` before boot | — (new) | The app owns board bring-up; fake-08 never sees it. |

## Deliberate divergences

All forced by the target (ESP32-S3 / ESP-IDF / a modern toolchain) and documented here; none edit `source/`.

- **D1 — flash cart, not a file.** `LoadCart(const unsigned char*, size, false)` with an embedded `.p8`
  string. *Why:* no filesystem in the draw-only milestone. Upstream also offers this overload
  (`vm.cpp:495`), so it's a config choice, not a code change.
- **D2 — timing via `esp_timer` + `vTaskDelay`.** Replaces SDL timing in `waitForTargetFps`. *Why:* no SDL;
  and yielding each frame feeds the FreeRTOS idle task / task watchdog.
- **D3 — input + audio stubbed.** `scanInput` returns 0; the audio trio no-ops. *Why:* parts-blocked (no
  I²C expander / MAX98357A yet). Mirrors bittboy's already-stubbed audio.
- **D4 — `drawFrame` rotates 90° CW.** *Why:* the board's display path presents PICO-8 content rotated 90°
  (bench-verified via the L-pattern; see the worklog). Contained here for now; the systemic fix (rotate
  once in `board.cpp`) is display backlog **`DP-8`**.
- **D5 — build: `-std=gnu++17`** (component-wide). *Why:* fake-08 is C++17; its global `lerp`
  (`mathhelpers.h:8`) collides with C++20 `std::lerp` under IDF's default. Build-level, in
  `components/fake08/CMakeLists.txt`.
- **D6 — build: lodepng disk I/O left enabled.** *Why:* `cart.cpp:310` references lodepng's *file*
  `decode` overload unconditionally; `LODEPNG_NO_COMPILE_DISK` would break the byte-identical `source/`.
  `fopen`/`fread` come from newlib; the path is dead code here.
- **D7 — z8lua = the shared `components/z8lua`** (`teapotlaboratories/z8lua @ pico-e32`), via `REQUIRES
  z8lua`; fake-08's nested `libs/z8lua` is left un-inited. *Why:* one VM, one place. Confirmed compatible —
  our fork exports the `lua_setpico8memory` API (`lua.h:260`) fake-08 needs (`vm.cpp:303`) and `fix32.h`.
- **Not-yet-done (noted):** the PSRAM Lua-heap hook the plan describes (`vm.cpp:300`) is unnecessary for
  boot — `CONFIG_SPIRAM_USE_MALLOC` already backs large allocations with PSRAM. Keeping the hot Lua
  state/GC nursery in internal SRAM is a **later optimization**, not done here.
