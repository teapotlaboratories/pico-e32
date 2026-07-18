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
| `components/fake08/fake08/platform/esp32/ESP32Host.cpp` | Implements fake-08's `Host` interface for the ESP32-S3 (draw, timing, input, SD cart scan) |
| `components/fake08/fake08/platform/esp32/fake08_board.h` | The `board_lcd_*` display contract (extern "C") |
| `components/fake08/CMakeLists.txt` | The **parent-repo** IDF wrapper: the vendored fork nests at `components/fake08/fake08/` and carries no build file of its own |
| `components/sdcard_spi/` | Reusable, board-agnostic microSD-over-SPI mount; the board supplies its wiring via `board_sd_config()` |
| `components/input/` | Compile-time-selectable input seam behind `scanInput` (`INPUT_BACKEND` = `stub`\|`serial`\|`touch`\|`i2c`) |
| `firmware/pico-e32-fake08/main/main.cpp` | `app_main` — the ESP-IDF entry replacing `source/main.cpp` (SD→flash cart ladder + input wiring) |
| `firmware/pico-e32-fake08/{CMakeLists.txt, main/CMakeLists.txt, sdkconfig.defaults}` | IDF project glue |

## `ESP32Host.cpp` ↔ fake-08

fake-08's `Host` is one concrete class (`source/host.h:60`); each platform supplies its method definitions.
`ESP32Host.cpp` defines the **23** methods **not** provided by the shared `source/hostCommonFunctions.cpp`
(which owns the other 13: palette/settings/filesystem). Reference platform = `platform/bittboy` (a simple
framebuffer device that outputs `uint16` RGB565 with stubbed audio — the closest analog).

| `ESP32Host.cpp` method | Declared | Modelled on (upstream) | Notes |
|---|---|---|---|
| `drawFrame(picoFb, screenPaletteMap, drawMode)` | `host.h:119` | `platform/bittboy/source/BittBoyHost.cpp:390` (default branch `:520`) | Same `_mapped16BitColors[screenPaletteMap[c]&0x8f]` LUT idea (`board_lcd_rgb565` LUT). Ours: **straight mapping**, `pico (x,y) -> (2x,2y)` matching bittboy/HG, 2× scaled, **blitted in 8 strips from a 16 KB internal-SRAM buffer** (a full 128 KB fb falls back to PSRAM → 16.5 ms; strips keep it internal → **3.6 ms**, measured). Nibble-unpack **inlined** (one byte → two px), not `getPixelNibble`. Only default `drawMode`. |
| `oneTimeSetup(Audio*)` | `host.h:103` | `BittBoyHost.cpp:196` (LUT build `:222`) | Build the RGB565 LUT from `_paletteColors` via `board_lcd_rgb565`; alloc the scaled fb; clear the panel. No SDL. |
| `setUpPaletteColors()` | — | `source/hostCommonFunctions.cpp:29` | **Reused as-is** (shared), fills `_paletteColors[144]` from `hostVmShared.h:11-44`. |
| `scanInput()` | `host.h:111` | `BittBoyHost.cpp:292` | Reads the compile-time input backend (`components/input` → `input_poll()` held mask) and computes the `KDown` edge; mouse/keyboard zeroed. Backend = `INPUT_BACKEND` (stub default; serial HITL-verified). See [input spec](pico-e32-fake08-input.md). Divergence D3. |
| `setTargetFps` / `waitForTargetFps` | `host.h:107,117` | `BittBoyHost.cpp:252,364` | `esp_timer` + `vTaskDelay` instead of `SDL_GetTicks`/`SDL_Delay` — divergence D2. |
| `shouldFillAudioBuff`/`getAudioBufferPointer`/`getAudioBufferSize`/`playFilledAudioBuffer` | `host.h:121-124` | `BittBoyHost.cpp:573-585` | **Stub** — identical to bittboy's already-stubbed audio. Divergence D3. |
| `shouldRunMainLoop`/`shouldQuit`/`changeStretch`/`forceStretch` | `host.h:109,112,114,115` | `BittBoyHost.cpp:588,360,256,283` | Loop always runs; no quit; stretch is a no-op (we own the 128→panel scale). |
| `listcarts`/`getCartDirectory` (impl); `listdirs`/`logFilePrefix`/`customBiosLua` (stub) | `host.h:130,144;146,133,135` | `platform/gcw0/source/ODHost.cpp:642` (listcarts) | `listcarts` scans `_cartDirectory` (the SD mount) for `.p8`/`.p8.png`, lower-casing names (defensive: matches any case, incl. an 8.3 fallback); `getCartDirectory` returns `_cartDirectory`. The other three still return empty/`""`. |
| `oneTimeCleanup`/`deltaTMs`/`overrideLogFilePrefix`/`setPlatformParams` | `host.h:126,128,132,154` | (bittboy `:237`; others desktop-only) | Cleanup frees the fb; the rest are inert no-ops. |

## `main.cpp` (`app_main`) ↔ `source/main.cpp`

The ESP-IDF entry mirrors fake-08's desktop `source/main.cpp:39-99` boot sequence, then hands off to
fake-08's **own** `Vm::GameLoop()` (`source/vm.cpp:860`) — the frame loop is fake-08's, not ours.

| `app_main` step | `source/main.cpp` | Notes |
|---|---|---|
| `new Host / new PicoRam / memory->Reset() / new Audio / Logger_Initialize / new Vm` | `:39-46` | Identical order. `Vm` ctor builds `Graphics`/`Input` and boots the Lua VM (`vm.cpp:300` `luaL_newstate`). |
| `setUpPaletteColors → oneTimeSetup → setTargetFps → SetCartList` | `:49-64` | Identical order. |
| Cart ladder: `LoadCart(sdPath)` else `LoadCart(cartBytes, len, false)` | cf. `:87` (`LoadCart` at `vm.cpp:495`) | **Divergence D1**: try an SD cart (the `std::string` overload → VFS file read from the onboard microSD) if one is mounted, else the flash-embedded `.p8` byte array (the `"pico"`-magic text path, `cart.cpp:398`). `FORCE_FLASH_CART` pins the flash cart. |
| `vm->vm_run()` | `:94` (`vm.cpp:1274`) | Starts the cart coroutine. |
| `vm->GameLoop()` | `:99` | Reused unchanged. |
| `board_lcd_init()` before boot | — (new) | The app owns board bring-up; fake-08 never sees it. |

## Deliberate divergences

All forced by the target (ESP32-S3 / ESP-IDF / a modern toolchain) and documented here; none edit `source/`.

- **D1 — cart source ladder (SD, else flash).** The app tries an SD cart (`LoadCart(std::string)` → VFS
  read from the onboard microSD via `components/sdcard_spi`) and falls back to a flash-embedded `.p8` byte
  array (`LoadCart(const unsigned char*, size, false)`). Both are upstream overloads (`vm.cpp:495`), so it's
  a config choice, not a code change. (A `FORCE_FLASH_CART` build flag pins the flash cart for bench demos.)
- **D2 — timing via `esp_timer` + `vTaskDelay`.** Replaces SDL timing in `waitForTargetFps`. *Why:* no SDL;
  and yielding each frame feeds the FreeRTOS idle task / task watchdog.
- **D3 — audio stubbed; input via a switchable seam.** The audio trio no-ops (parts-blocked: no MAX98357A).
  `scanInput` is **no longer a stub** — it reads a compile-time-selected input backend (`components/input`):
  the `serial` backend is HITL-verified on hardware, `touch` (FT6236, on-board) and `i2c` (expander,
  parts-blocked) are skeletons. Default backend is `stub` (input off), so a plain build still boots untouched.
- **D4 — retired (not a divergence).** An earlier revision rotated `drawFrame` 90° CW, believing the
  display was rotated. It isn't — the **bench camera** is mounted 90° (`docs/…/pico-e32-bench-camera.md`,
  bench-rig-gotchas: raw captures must be turned 90° CW to judge). `drawFrame` is the straight mapping,
  identical in orientation to bittboy/HG. Slot kept so D5–D7 don't renumber.
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
