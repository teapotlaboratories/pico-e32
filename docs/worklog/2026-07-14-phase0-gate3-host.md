# 2026-07-14 — Phase-0 Gate #3: trivial cart end-to-end (minimal ESP32Host)

**Goal:** tie the pieces together — `z8lua` (runtime) + `components/ili9488` (display) + a
minimal PICO-8 draw API + a trivial cart — into a real frame loop, at **≥ 30 fps @ 256²**.

**Status:** ✅ **groundwork passed (camera-independent).** Rendering verified byte-identical
host↔device; full pipeline runs **161.5 fps**. Only the physical-panel image is unverified (needs the camera).

---

## What was built — `firmware/pico-e32-host/`

The seam that grows into the full ESP32Host (Phase-1 fake-08 port). One file
(`main/host_main.cpp`):
- **128×128 indexed framebuffer** + PICO-8 palette (RGB565, byte-swapped for the i80 bus).
- **Minimal PICO-8 draw API** (C, writing to the framebuffer): `cls`, `pset`, `color`,
  `rectfill`, `rect`, `circfill`, `circ`, `line`; no-op stubs for `spr`/`print`/`map`/etc.
- **A trivial cart** (embedded Lua): animated `circfill`s + `rectfill`s + a moving `pset`/`line`,
  using only the draw API + z8lua's `sin`/`cos`.
- **Frame loop:** `_update`→`_draw` (into the framebuffer) → palette-expand + integer 2× scale to
  256×256 RGB565 → `ili9488_blit`, with FPS + a per-frame framebuffer checksum over UART.

The same file compiles **host-standalone** (`-DHOST_MAIN`, no display) to run the cart + draw API
and print the reference checksums — enabling fast host iteration (found + fixed a Lua-stack bug in the
`_update`/`_draw` call path under ASan before ever flashing).

## Verification — framebuffer checksum (no camera needed)

`fb_hash` = FNV-1a over the 128×128 framebuffer, printed per frame. Because the cart, z8lua fix32
math, and the draw API are deterministic, **host and device must produce the identical sequence** if
the render pipeline is correct.

```
host   frames 0-9: 432b2a8d b99c6cb6 a3304eb4 58e93d00 2d861cfc afd99258 a8a681d0 a5fb37c3 01d10580 2e41c322
device frames 0-9: 432b2a8d b99c6cb6 a3304eb4 58e93d00 2d861cfc afd99258 a8a681d0 a5fb37c3 01d10580 2e41c322   ← identical
```

**Byte-for-byte match** → the cart's pixels reach the framebuffer correctly on hardware. The hashes
also change every frame → the cart is animating, not static.

## Result

- **161.5 fps** sustained for `_update` + `_draw` + scale + 256×256 blit. ~5.4× the 30 fps gate,
  ~2.7× the 60 fps target.
- (Raw blit alone was 288 fps in Gate #1; the cart draw + scale add ~2.7 ms/frame → 161 fps. Expected.)
- App = `0x5c590` (378 KB), 64% partition free. Frame loop yields on the blit's DMA wait, so the Task
  WDT is happy (no disable needed, unlike the tight benchmark loop).

## What's verified vs not

- ✅ Cart runs end-to-end; draw API produces **correct pixels** (host==device); pipeline ≥ 30 fps.
- ⚠️ **Not** verified: the physical panel actually shows it (colours/orientation on glass) — same
  camera blocker as Gate #1.

## Next

- Camera → confirm the panel image (closes Gate #1 + this).
- Phase 1: replace the trivial cart with the fake-08 host + a real cart from SD; add input + I²S audio.
- Consider extracting the draw API into a reusable `components/p8gfx` when it grows.
