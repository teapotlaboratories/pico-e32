# 2026-07-14 — Phase-0 Gate #1: ILI9488 i80 scaled-blit FPS

**Goal:** bring up the display-test on real hardware and measure the 256×256 scaled-blit
frame rate. **Gate #1: ≥ 30 fps @ 256²** on the Makerfabs ILI9488 (16-bit i80 parallel).

**Status:** ❌ **VOID — see [2026-07-16](2026-07-16-panel-rev1-pinmap.md).**

> **The 288 fps below is not a display measurement.** The pin map used here (WR=18, DC=17, CS=46) is
> for Makerfabs' *newer* board; this unit is **rev 1**, whose LCD is on **WR=35, DC=36, CS=37**. The
> i80 bus was clocking into pins that are not wired to the panel — the panel showed **nothing** for the
> entire time this "pass" stood. The bus-utilisation arithmetic here is still sound; it just measures
> DMA into unconnected pins.
>
> This log's own warning turned out to be exactly right — *"a broken panel init could still clock DMA at
> 288 fps into a blank screen"* — and it was written off as a hypothetical.

---

## Hardware / build

- Same board on `/dev/ttyUSB0` — ESP32-S3 N16R2 (Makerfabs ILI9488 Parallel TFT). 2 MB PSRAM.
- `make build APP=pico-e32-display-test BOARD=makerfabs-ili9488` → clean, app = `0x384e0` (231 KB), 78% partition free.
- `make flash … PORT=/dev/ttyUSB0` → hash verified, hard reset.

## Result (UART)

```
I (837) trackA: palette bars drawn at (32,112) 256x256
I (5847) trackA: ==== Gate #1: 1441 frames in 5.00s = 288.0 fps (256x256 blit) ====
I (5847) trackA: PASS if >= 30 fps.
```

**288 fps @ 256²** — passes Gate #1 (≥ 30 fps) by ~10×.

**Sanity — is 288 fps real?** Yes, it's physically consistent:
- 288 fps × (256×256×2 B) = **37.7 MB/s**.
- i80 bus = 20 MHz pclk × 16-bit = **40 MB/s** ceiling → 37.7 = **~94% utilization**, ~305 fps theoretical max.
- `draw_block()` **blocks on DMA completion** (semaphore from the `on_color_done` ISR) each frame, so this is a true completed-transfer rate, not a spinning loop. The init sequence ran with no `ESP_ERROR_CHECK` abort (the app reached the FPS loop), so the i80 bus + panel IO are configured correctly.
- The in-code comment "~130 fps theoretical @16-bit/20MHz" is **wrong** — actual ceiling is ~305 fps; 288 is right against that.

**Headroom:** the real use case is a 128×128 → 2×-scaled (256²) letterboxed blit at 30 fps — that needs only ~10% of this. Plenty of margin even before double-buffering.

## ⚠️ Blocker: visual correctness not verified

Per `.ai/AGENTS.md`, display changes must be confirmed by the **bench camera** — and it is
**unavailable here**: no `/dev/video*`, no capture tooling, no `docs/hardware/` capture doc; the
plan's intended capture cam is an **ESP-EYE** that isn't set up. So I measured *throughput* but
**cannot confirm what's on the glass** — colours, RGB/BGR order (`SWAP_COLOR_BYTES`), MADCTL
orientation, or whether the panel is blank/garbled. The driver is explicitly UNVERIFIED (see the
header in `main/main.c`), so this gap is real: a broken panel init could still clock DMA at 288 fps
into a blank screen.

**What's confirmed:** bus/DMA throughput and that init didn't fault. **What's not:** that the
correct pixels are displayed.

## Follow-up: extracted the driver into a reusable component (camera-independent)

The ILI9488 driver was inline in the test harness. Extracted it to **`components/ili9488`**
(`ili9488.h` / `ili9488.c` / `CMakeLists.txt`) so Gate #3 and the future `ESP32Host` can share one
driver instead of copy-pasting:
- API: `ili9488_init(cfg)` (bus + panel IO + init sequence + backlight), `ili9488_blit(x,y,w,h,src)`
  (blocks on DMA completion), `ili9488_fill(color)`, `ili9488_rgb565(r,g,b)`.
- Board specifics (pins, `pclk`, `madctl`, `swap_color_bytes`, resolution) come in via
  `ili9488_config_t`, so the driver is board-agnostic; the Makerfabs pin map now lives in the test
  harness (`main.c`).
- The harness (`main.c`) keeps only the PICO-8 palette, test image, 2× scale, and FPS loop.

**Verified:** rebuilt + reflashed → **identical 288.0 fps / 1441 frames** (byte-for-byte the same
UART output), so the refactor is behavior-preserving at the bus level. No camera needed for this.

## Next

- **Set up the capture camera** (ESP-EYE or a USB webcam → `docs/hardware/` capture doc), then
  flash → capture → inspect the palette bars; adjust `swap_color_bytes` / MADCTL if colours or
  orientation are wrong. Only then is Gate #1 *fully* passed.
- After visual confirmation: Gate #3 (trivial cart end-to-end on the panel), reusing `components/ili9488`.
