# 2026-07-17 — fake-08 port: vendoring + the ESP32 platform scaffold

The runtime is a **port of fake-08** (`jtothebell/fake-08`, MIT) — see
[the port plan](../runtime/pico-e32-fake08-port.md) and [`.ai/AGENTS.md` → primary goal].
This log tracks bringing fake-08 into the tree and standing up an `esp32`/`pico-e32` platform.
Phase-0 (display + z8lua + the HG de-risking harness) is done and on `main`; this starts Phase 1.

## Decision — how to vendor (settled with the owner)

**Fork into the org and follow fake-08's per-system convention.** fake-08 already carries per-system
port branches off `master` (`funkey-build`, `wiiu-standalone`, `rg35xx-libretro`,
`xbox-series-libretro`, …) and a `platform/<system>/` dir per target. So:

- **Fork:** `jtothebell/fake-08` → **`teapotlaboratories/fake-08`** (done — master only). Same pattern as
  the existing `teapotlaboratories/z8lua` fork.
- **Branch:** a project branch **`pico-e32`** off `master` (matches the `z8lua` fork's `pico-e32` branch).
- **Platform:** add **`platform/esp32`** (its own build → the ESP32-S3 library for the system), leaving
  `source/` **byte-identical** (the 1-to-1 rule). New `Host` code is additive, exactly like every other
  `platform/` dir; we simply don't compile the other platforms.
- **z8lua:** reconcile fake-08's nested `libs/z8lua` submodule (→ `jtothebell/z8lua`) onto our
  **`teapotlaboratories/z8lua @ pico-e32`** — one VM, one place. The PSRAM heap hook lives in that fork's
  allocator, so fake-08's `vm.cpp:300` `luaL_newstate()` stays untouched.
- **Consume:** add `teapotlaboratories/fake-08 @ pico-e32` as a submodule at `components/fake08` in this repo.

Why not a pristine-upstream submodule + wrapper (the other option): the owner wants convention-consistency
with z8lua, and a fork gives a clean home for the `platform/esp32` build + the reconciled `libs/z8lua`
pointer. `source/` still stays byte-identical, so the 1-to-1 rule holds either way.

## Grounded facts (from a scout clone of `jtothebell/fake-08 @ master`)

- `source/vm.cpp:300` is exactly `_luaState = luaL_newstate();` — the PSRAM heap seam the plan cites. ✅
- `source/host.h` is the single concrete `Host` class (drawFrame / setTargetFps / waitForTargetFps /
  deltaTMs / scanInput / the audio pull trio / cart I/O). Each `platform/<sys>/` supplies the definitions.
- `libs/` = `lodepng`, `miniz`, `simpleini`, and `z8lua` (a **nested submodule** → `jtothebell/z8lua`).

## Build architecture (grounded in the existing apps)

Each app is an IDF project under `firmware/<app>/` whose project `CMakeLists.txt` sets
`EXTRA_COMPONENT_DIRS` → `../../components`, and whose `main/CMakeLists.txt` `REQUIRES` the shared
components (`z8lua`, `LovyanGFX`, `driver`, `esp_timer`) and compiles `${BOARD_DIR}/board.cpp` into `main`.
Boards own their display driver + `sdkconfig.defaults` (target + PSRAM); `make build APP=… BOARD=…` layers
board config first, then the app's. So the port adds:

- **`components/fake08`** (the submodule) — an IDF component (root `CMakeLists.txt` on the `pico-e32`
  branch) globbing `source/*.cpp` + `platform/esp32/*.cpp`, `REQUIRES z8lua`.
- **`firmware/pico-e32-fake08`** — a new app: project CMakeLists + `main/` (app_main + board wiring + the
  flash cart), `REQUIRES fake08 z8lua LovyanGFX driver esp_timer`.

## z8lua + PSRAM — simpler than the plan feared

- `components/z8lua` exports headers via `INCLUDE_DIRS "."`, so `REQUIRES z8lua` is all fake-08 needs to
  compile against **our** VM (no `libs/z8lua` copy). One VM, one place.
- The board enables `CONFIG_SPIRAM=y` + `CONFIG_SPIRAM_USE_MALLOC=y` (2 MB quad PSRAM, N16R2). So
  `luaL_newstate()` → `l_alloc` → `realloc()` (z8lua `lauxlib.c:919/937`) already draws large Lua
  allocations from the PSRAM-backed combined heap — **no `vm.cpp:300` edit needed for the boot.** The
  "hot state/nursery in internal SRAM" split from the plan is a **later optimization**, not a milestone
  blocker; deferred and noted.
- **Real risk (build-time):** fake-08 pins `jtothebell/z8lua`; we link `teapotlaboratories/z8lua @
  pico-e32`. If the fork's API diverged (fix32, custom libs), fake-08's `source/` may not compile against
  ours. This is the plan's flagged reconciliation risk — it surfaces at first build.

## Progress

- ✅ Forked `jtothebell/fake-08` → `teapotlaboratories/fake-08` (master only).
- ✅ Created the `pico-e32` branch on the fork (off `master` `814991a`).
- ✅ Added it as submodule `components/fake08` (tracks `pico-e32`); nested `libs/z8lua` left un-inited.
- ✅ Grounded the build architecture + z8lua/PSRAM story (above).
- ✅ Recon brief produced (subagent, all cited): Host method inventory, `drawFrame` nibble-unpack,
  boot sequence, cart-load path, memory map. Key findings folded in below.
- ✅ Authored the port (7 files). First build kicked off.

## What the brief settled (the load-bearing facts)

- **`picoFb` is not a separate buffer** — it's `PicoRam.screenBuffer` (offset 0x6000), 128×128 **4-bpp,
  2 px/byte, 64-byte stride**; even-x = low nibble. Unpack via `getPixelNibble(x,y,buf)`
  (`nibblehelpers.cpp:29`). Pixel colour = `LUT[ screenPaletteMap[c] & 0x8f ]`.
- **No `UpdateAndDraw`** — the driver is `Vm::Step()` (runs one `_update`/`_draw` *inside* Lua via
  `__z8_tick`), then read `GetPicoInteralFb()` + `GetScreenPaletteMap()` and `drawFrame(...)`. This is
  exactly `Vm::GameLoop()` (`vm.cpp:860`) — so the **most faithful port reuses `GameLoop()`**, not a
  reimplemented loop.
- **Cart load:** `vm->LoadCart(const unsigned char*, size, false)` — the ctor auto-detects the `"pico"`
  magic and parses `.p8` **text** (`cart.cpp:398`). Then `vm->vm_run()` starts the coroutine. So a
  hand-written `.p8` string embedded in flash is the smallest path — no PNG/lodepng needed.
- **`Host` split:** `source/hostCommonFunctions.cpp` defines 13 methods (fs/palette/settings);
  `ESP32Host.cpp` defines the other 23 (display + timing + input/audio stubs).
- **z8lua seam confirmed present in our fork:** `lua_setpico8memory` (`lua.h:260`) + `fix32.h` — fake-08
  links against `components/z8lua` via `REQUIRES z8lua`. `Logger_Initialize` is `#if LOGGER_ENABLED`
  (off) → inert, no FS access. `source/` has **no SDL/dirent/glob** includes → compiles on ESP-IDF.

## Files authored

On the **fork** (`components/fake08 @ pico-e32`), `source/` untouched:
- `CMakeLists.txt` — IDF component: `source/*.cpp` (minus `main.cpp`) + `platform/esp32/ESP32Host.cpp`
  + `libs/{lodepng,miniz,simpleini}`, `REQUIRES z8lua esp_timer`, `LODEPNG_NO_COMPILE_DISK`.
- `platform/esp32/ESP32Host.cpp` — the 23 Host methods. `drawFrame`: nibble-unpack → RGB565 LUT →
  integer **2× to 256×256, centred** on the 320×480 panel → one `board_lcd_blit`. `waitForTargetFps`:
  `esp_timer` + `vTaskDelay` (feeds the watchdog). Input/audio stubbed.
- `platform/esp32/fake08_board.h` — the `board_lcd_*` display contract (extern "C", matches `board.h`).

In the **pico-e32 repo** (`firmware/pico-e32-fake08/`):
- `main/main.cpp` — `app_main`: `board_lcd_init()`, then fake-08's boot sequence, then `vm->GameLoop()`.
  Embeds a tiny **hand-written `.p8` test cart** (ours): 16 colour bars + `print` + a frame counter —
  exercises `cls`/`rectfill`/`print`/`_update`/`_draw` with no `__gfx__`.
- `CMakeLists.txt` (project) + `main/CMakeLists.txt` (`REQUIRES fake08 z8lua LovyanGFX driver esp_timer`)
  + `sdkconfig.defaults` (32 KB main stack for the Lua pcall path).

- ✅ First build got to **91%** — **the z8lua-fork reconciliation HELD.** fake-08's `vm.cpp`,
  `graphics.cpp`, `picoluaapi.cpp`, `Audio.cpp` etc. all compiled against `components/z8lua` (our
  `teapotlaboratories/z8lua @ pico-e32` fork) with no API drift. The big risk is retired.
- ✅ **One real compile error, fixed:** `cart.cpp:310` calls `lodepng::decode(image,w,h,filename)` — the
  *file* overload — which my `LODEPNG_NO_COMPILE_DISK` define (from the plan) compiled out. fake-08's
  `cart.cpp` references it unconditionally, so disabling disk I/O breaks the byte-identical `source/`. Fix:
  **keep lodepng disk I/O enabled** (removed the define). `fopen`/`fread` come from newlib; the file path
  is never taken (we load carts from memory). The plan's `LODEPNG_NO_COMPILE_DISK` suggestion was wrong
  for a byte-identical port — noted for the code-map.
- ✅ **Second compile error, fixed:** `mathhelpers.h:8` global `lerp(float,float,float)` collides with
  **C++20 `std::lerp`** (`graphics.cpp` does `using namespace std`; IDF 5.4 defaults to gnu++2b, so
  `<cmath>` now declares `std::lerp`). fake-08 is a C++17 codebase. Fix: compile the fake08 component at
  **`-std=gnu++17`** (its intended standard; last `-std` wins over IDF's default) — no source edit.
- ⏳ Rebuilding after the gnu++17 fix.

## Build-integration fixes (for the code-map — all keep source/ byte-identical)

The plan's "author an IDF component" step meets a modern toolchain (GCC 14.2, C++23 default). Two forced,
build-level adaptations so upstream `source/` compiles unchanged:

1. **lodepng disk I/O left enabled** — `LODEPNG_NO_COMPILE_DISK` (plan §1) breaks `cart.cpp:310`'s
   `loadCartFromPng(filename)` file overload. Kept enabled; the file path is dead code here.
2. **`-std=gnu++17` on the fake08 component** — fake-08 predates C++20's `std::lerp`; its global `lerp`
   clashes under IDF's newer default.

Both live in `components/fake08/CMakeLists.txt` (the fork's build wrapper), not in `source/`.

## Milestone reached — fake-08 boots and renders on the panel 🎉

Built (846 KB, 19% partition free), flashed to `/dev/ttyUSB1`, camera-captured. **The panel shows a tiny
hand-written `.p8` test cart running under fake-08:** 16 PICO-8 palette colour bars, the text
"FAKE-08 ON ESP32-S3", and a live "FRAME nnn" counter (so `_update` *and* `_draw` run through fake-08's
own `Vm::GameLoop`). Palette and the built-in font are correct. **This retires the plan's one real unknown
— "nobody has run fake-08 on an ESP32 specifically."** It does.

## Orientation finding — the panel renders PICO-8 content rotated 90°

The first capture was **rotated 90°** (cart draws vertical bars + horizontal text; panel showed horizontal
bars + vertical text). Ruled out a camera move by reflashing the trivial L-pattern host and capturing in
the *same* camera position — it matched the archived `lpattern-after.jpg` exactly (camera unchanged).

Root cause: the board's display path presents PICO-8 content **rotated 90°**, and the L-pattern cart never
revealed it — it draws a top bar + left bar ("Γ") that happens to render as an on-panel "L", which *looks*
plausibly upright. fake-08's **text** is the first content that makes the rotation obvious. HG and fake-08
share the identical blit (`host_main.cpp:538-541` == `ESP32Host::drawFrame`), so this is a property of the
whole display path, not fake-08-specific.

Fix (contained, in `ESP32Host::drawFrame`): **pre-rotate 90° CW** — `pico (x,y) -> buf(row=2x,
col=255-2y)` — so carts render upright. **✅ Verified on the bench:** after the fix the colour bars are
vertical (as drawn) and the text reads left-to-right ("FAKE-08 ON ESP32-S3" / "FRAME nn"). Capture saved
as `fake08-milestone-upright.jpg`.

The *systemic* fix (rotate once in `board.cpp` so every app agrees, instead of per-`drawFrame`) is a
follow-up — logged as **`DP-8`** in the display doc. The HG apps share the same latent rotation.

## Status — draw-only milestone DONE

All five port tasks complete. fake-08 runs on the ESP32-S3: shared z8lua VM, flash `.p8` cart, upright
render, animating. Landed: fork `pico-e32` branch pushed (`teapotlaboratories/fake-08@92e39de`), the
`pico-e32` app + submodule committed. Parts-blocked seams (input, audio, SD) are the next phase; a code-map
(`docs/runtime/pico-e32-fake08-codemap.md`) records the new-code ↔ fake-08 mapping + the deliberate
divergences.
