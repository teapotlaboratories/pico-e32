# 2026-07-17 — fake-08 on-device fps: measurement → a 4.6× drawFrame fix

Follow-up to the merged [fake-08 draw-only port](2026-07-17-fake08-port-vendor.md). The port renders, but
**nobody had measured its real on-device frame time** — Gate #2 (`luabench`) measured the z8lua VM with
*every draw call stubbed*, so the actual `Step()` + `drawFrame()` cost was unknown. This is the load-bearing
number for the 30-vs-60 fps policy. Measured it; found and fixed a real bottleneck.

## How it was measured

An opt-in build — `make build APP=pico-e32-fake08 … DEFS='-D CELESTE=1 -D MEASURE_FPS=1'` — replaces
fake-08's `GameLoop` with an instrumented loop (in the app, `firmware/pico-e32-fake08/main/main.cpp`) that
times **`vm->Step()`** (the VM's `_update`/`_draw`) vs **`host->drawFrame()`** (our unpack → RGB565 → 2×
scale → blit) separately with `esp_timer`, paced to the target fps, logging every 30 frames over UART.
Read the UART with a raw `stty 115200` + `cat` (idf.py monitor won't run headless).

Target = real **Celeste** (the actual `.p8`, opt-in). It parks on the title (input stubbed), so the `Step`
number here is the *title* screen (light); the `drawFrame` number is **content-independent** (it always
processes all 128×128 px + one 256×256 blit), so it's representative regardless.

## The finding — drawFrame was PSRAM-bound

| | step (ms) | drawFrame (ms) | total (ms) | ceiling |
|---|---|---|---|---|
| **before** | 1.9 | **16.5** | 18.4 | 54 fps |
| **after** | 1.7 | **3.6** | 5.3 | **189 fps** |

`drawFrame` was **16.5 ms — ~9× the VM cost.** Boot log gave it away: `scaled fb fell back to PSRAM`,
`fb=0x3c1004f8` (PSRAM is `0x3C000000`+; internal DRAM is `0x3FC88000`+). The 256×256 scaled framebuffer is
**128 KB**, which doesn't fit internal DMA-capable SRAM, so it fell back to PSRAM — and every per-pixel
write **and** the blit-read then went through slow PSRAM.

## The fix — blit in strips from internal SRAM

Don't materialise a 128 KB framebuffer. Build + blit the frame in **8 strips of 32 panel rows** through a
**16 KB internal DMA buffer** (`strip=0x3fcc99c8` — internal, no fallback). All per-pixel writes and the
blit DMA now stay in fast internal SRAM. Also **inlined the 4-bpp nibble-unpack** (read one byte → two
pixels) to drop the per-pixel `getPixelNibble` call. Lives in the fork's `ESP32Host::drawFrame`
(`teapotlaboratories/fake-08@8f4b6a6`), `source/` still untouched.

**Result: drawFrame 16.5 → 3.6 ms (4.6×); frame ceiling 54 → 189 fps.**

## What it means for frame rate

`drawFrame` is now 3.6 ms and content-independent. Combined with Gate #2's measured gameplay VM cost
(~15.8 ms avg, 5–40 ms per room):

- **avg room** ≈ 15.8 + 3.6 = **~19 ms → ~51 fps** — comfortable 30, near 60
- **light rooms** ≈ 8 ms → **60 fps capable**
- **densest rooms** ≈ 40 + 3.6 = ~44 ms → ~23 fps — now **VM-bound**, not draw-bound

So the draw path is no longer the fps concern; frame rate is now bounded by the **z8lua interpreter on
heavy rooms** — i.e. the open [z8lua-speedup](../reference/z8lua-speedup-research.md) question, to be tackled
only after profiling a representative cart on the S3.

## Follow-ups

- A **direct gameplay `Step` measurement** (drive Celeste past the title via `scanInput`) would firm up the
  ~15.8 ms extrapolation — currently the `Step` number measured is the title screen.
- `drawFrame` could inline further / use 32-bit stores, but at 3.6 ms it's no longer the bottleneck.
