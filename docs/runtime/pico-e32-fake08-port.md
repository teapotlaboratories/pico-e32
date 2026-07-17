# ESP32Host — porting fake-08 (the primary runtime goal)

**The runtime is a port of [fake-08](https://github.com/jtothebell/fake-08)** (`jtothebell/fake-08`, MIT —
the reference open-source PICO-8 player), **not a from-scratch implementation.** We take fake-08's whole
PICO-8 runtime (the Lua binding, the entire draw/audio/API, the cart loader) and replace **only its `Host`
layer** — display, input, audio, storage, timing — for the ESP32-S3. See
[`.ai/AGENTS.md` → Primary goal](../../.ai/AGENTS.md) and its **1-to-1 porting rule**: every ported unit
matches fake-08 function-for-function unless a target constraint forces a *documented* divergence.

This doc is the **concrete, current-state** plan. The strategy + verified source notes live in the plan of
record, [`docs/pico-e32-development-plan.md`](../pico-e32-development-plan.md) §5 — read that too; this
doesn't duplicate it.

## Why a port, not the hand-written draw API we have

The `pico-e32-host` app grew a **from-scratch** PICO-8 draw API (`spr`/`map`/`print`/`pal`/`camera`, the
`HG-*` items in [`pico-e32-host-graphics.md`](pico-e32-host-graphics.md)). That was a legitimate **Phase-0
de-risking / verification harness** — it proved z8lua + our display render real Celeste content,
camera-free, and surfaced real findings (the map/sprite shared-memory aliasing, the font question, the
16-bit byte order). **But it is not the runtime.** fake-08 already implements all of it (and audio, the
full API, P8SCII, every edge case), tested. Per the 1-to-1 rule, the hand-written draw code is now
**reference/verification only — to be superseded by fake-08's own graphics, not extended.** What carries
forward is the *hardware* work (the display driver, z8lua, the frame-dump harness) and the *findings*.

## What fake-08 gives us vs what we write

fake-08's `Host` is a **single concrete class** (`source/host.h`); each platform links its own definitions.
We write one **`ESP32Host`**. Everything else is fake-08's, ported as-is.

| Host responsibility | fake-08 signature (`source/host.*`) | our ESP32 implementation | status |
|---|---|---|---|
| **Framebuffer flush** | `drawFrame(uint8_t* picoFb, uint8_t* paletteMap, uint8_t drawMode)` — `picoFb` = native **128×128 4-bpp indexed** (8 KB) | nibble→RGB565 line-expand + 2× scale → `board_lcd_blit` (the board display driver we already have) | 🟢 **ready** — driver done (393 fps, upright); the scale/blit exists, needs the **4-bpp/nibble** unpack (our HG harness fb is 8-bpp) |
| **Timing** | `setTargetFps`, `waitForTargetFps`, `deltaTMs` | `esp_timer` + `vTaskDelay` (already used in Gate #1/#3) | 🟢 **ready** |
| **Lua VM heap** | `luaL_newstate()` at `source/vm.cpp:300` | swap to `lua_newstate(psram_alloc,…)` with `MALLOC_CAP_SPIRAM`; keep `picoFb`, DMA buffers, GC nursery in **internal SRAM** | 🟢 **ready** — z8lua already vendored (`components/z8lua`), the VM fake-08 uses |
| **Cart source** | `getFileContents`, `listcarts`, `saveCartData`, … | **now:** embed one cart in flash (like `celeste_cart.h`). **later:** SD (FatFs) / LittleFS via VFS | 🟡 flash-cart ready; SD is **parts-blocked** (no microSD yet) |
| **Input** | `InputState_t scanInput()` → 8-bit mask (L/R/U/D/O/X/PAUSE) | read I²C GPIO expander → OR the bits. **now:** BOOT button / stub | 🔴 **parts-blocked** (no I²C expander/buttons yet) |
| **Audio** | `Audio::FillAudioBuffer(...)` @ **22050 Hz S16**, the **pull/poll** path | ESP-IDF **I²S** feeder task → MAX98357A. **now:** stub | 🔴 **parts-blocked** (no MAX98357A/speaker yet) |

**Key point:** *draw + timing + VM + a flash cart are all* 🟢 *ready — none are parts-blocked.* So the
first real port milestone can happen **now**, on the bench, without any parts order.

## What we already have (that the port reuses)

- **z8lua** — `components/z8lua` (submodule). This is exactly the VM fake-08 pulls (`libs/z8lua`). One
  z8lua for both; reconcile fake-08's `libs/z8lua` to our submodule rather than vendoring a second copy.
- **Display** — `boards/makerfabs-ili9488-r1/board.{h,cpp}` (`board_lcd_init/blit/fill/rgb565`), verified
  upright + correct at 393 fps. This *is* `drawFrame`'s back end.
- **`esp_timer`** — the timing primitive for the `setTargetFps`/`deltaTMs` methods.
- **The frame-dump harness** (`HG-2`, `P8_DUMP` → PNG + the host↔device `fb_hash` compare) — reusable to
  verify **fake-08's** `picoFb` the same camera-free way we verified the hand-written draw API.
- **The HG findings** — the map/sprite shared-memory aliasing, the CC-0 font question, the byte-order
  crossover. These inform the port even though the *code* is superseded.

## Port steps (per plan §5, verified from fake-08 source)

1. **Vendor fake-08.** No CMake in fake-08 (per-platform GNU Makefiles only) → **author our own IDF
   component** `CMakeLists.txt` listing `source/*.cpp`, our `z8lua` (compiled **as C++**, `-DLUA_USE_LONGJMP`
   — `fix32` is a C++ type), and `libs/{lodepng,miniz,simpleini}`. Define `LODEPNG_NO_COMPILE_DISK`.
   `utf8-util` is a dangling Makefile reference (absent, unused) — ignore it. **1-to-1:** keep fake-08's
   `source/` byte-identical; put all of this in the build, not edits (least-destructive rule).
2. **PSRAM heap.** Replace `luaL_newstate()` at `source/vm.cpp:300` with `lua_newstate(psram_alloc,…)`
   (`heap_caps_*`, `MALLOC_CAP_SPIRAM`). **This is a forced, documented divergence** — record it in the
   port's code-map. Keep the hot Lua state + GC nursery, `picoFb`, RGB565 line buffer, and **all DMA
   descriptors** in internal SRAM.
3. **Write `ESP32Host`.** Implement fake-08's `Host` interface with the table above — draw via
   `board_lcd_*`, timing via `esp_timer`, a flash cart for `getFileContents`, stubs for input/audio until
   the parts land.
4. **Code-map doc.** A substantial port ships a function-level new-code ↔ fake-08 map with a
   deliberate-divergences section (per AGENTS.md → Porting). The `ESP32Host` methods and the `vm.cpp:300`
   heap hook are the first entries.

## First milestone — draw-only, no parts (do this next)

Per plan §0.5 / development-plan.md:174: **a minimal `ESP32Host` (only `drawFrame` + timing; input/audio
stubbed) running a flash-embedded cart on the panel.** This:
- **De-risks the real unknown** the plan flags: *"nobody has run fake-08 on an ESP32 specifically"* — so
  getting fake-08's actual runtime to boot + render on the S3 is the load-bearing test.
- Is **directly comparable** to the HG work: run the same Celeste room through *fake-08's* draw path,
  dump `picoFb` → PNG, and check it against the HG dumps + the panel. If it matches, the port is real.
- Needs **no parts** — it's the same draw+timing+VM+flash-cart we already have, just fake-08's runtime
  instead of the hand-written one.

Then Phase 1 fills in the parts-blocked seams (input, audio, SD) → **Gate #4** (a real cart playable ≥30 fps
with sound + input).

## Open questions / risk

- **fake-08 on ESP32 is unproven** (plan caveat). tac08 and PicoPico *are* proven on ESP32 — if fake-08
  fights the S3 badly, they are the fallback bases. The draw-only milestone is what tells us.
- **Heap:** does fake-08 statically reserve a large block, or grow dynamically? Plan §5 (`vm.cpp:300`) says
  dynamic — confirm on the port.
- **z8lua reconciliation:** ensure fake-08 builds against our existing `components/z8lua` submodule, not a
  second copy — one VM, one place.
- **Bytecode precompile** (PicoPico's `to_c.py`, parse 112 ms → 18 ms): build-time vs load-time — a Phase-1
  decision (open in the master TODO).
