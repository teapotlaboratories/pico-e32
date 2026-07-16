# Track A — ILI9488 scaled blit + FPS (Phase 0, Gate #1)

Proves the display path on the Makerfabs 3.5" ILI9488 board and measures how fast a scaled
PICO-8 frame can be pushed over the 16-bit i80 bus.

- **A1** — draw 16 PICO-8 palette bars (a 128×128 indexed image) → confirms the bus, pin
  map, orientation and colour order. Verify with the bench camera.
- **A2** — palette-expand + integer **2× scale to 256×256**, centred with black borders,
  redrawn in a loop → prints sustained **FPS** over UART.

**🚦 Gate #1: ≥ 30 fps** for the 256×256 blit. (Bus theoretical ≈ 130 fps at 16-bit/20 MHz,
so this should pass comfortably; if not, it's a pclk/DMA config issue, not the panel.)

## Build
```sh
# from the repo root (ESP-IDF v5.1+):
make build flash monitor APP=pico-e32-display-test BOARD=makerfabs-ili9488-r1
```

## ⚠️ Not yet run on hardware
**Compiles clean for esp32s3** with the vendored ESP-IDF v5.4.2 (`make build` → 231 KB image),
so the `esp_lcd` i80 API usage and pin config are sound against real headers. What's
**unverified** is on-hardware behaviour — chiefly the **ILI9488 init sequence**, orientation
and colour order (these can only be confirmed on the panel, via the bench camera). The pin map and
the palette-expand → 2× scale → FPS logic are the high-confidence parts.

If the panel is blank or colours/orientation are wrong, in `main/main.c`:
- **Colours swapped / wrong** → flip `SWAP_COLOR_BYTES`, or adjust the `0x36` **MADCTL**
  byte (BGR/RGB + orientation).
- **Blank panel** → cross-check against the vendor's known-good **LovyanGFX** example
  ([github.com/Makerfabs/Makerfabs-ESP32-S3-Parallel-TFT-with-Touch](https://github.com/Makerfabs/Makerfabs-ESP32-S3-Parallel-TFT-with-Touch));
  the pin map here matches its config (`freq_write = 20 MHz`, WR=18, RD=48, RS=17, CS=46,
  BL=45, and the 16 data pins). The init sequence is the usual ILI9488 set — swap in the
  vendor's exact gamma/power bytes if colour/contrast is off.

## Pin map (from the vendor LovyanGFX config)
| Signal | GPIO |
|---|---|
| Data D0–D15 | 47,21,14,13,12,11,10,9, 3,8,16,15,7,6,5,4 |
| WR / RD | 18 / 48 |
| DC (RS) | 17 |
| CS | 46 (board ties low) |
| Backlight | 45 (PWM) |

## What this does *not* do
It draws a full 256×256 block per frame for a simple FPS number. The real runtime keeps the
128×128 indexed framebuffer in SRAM and expands + scales **line by line** into a small
buffer (no full framebuffer in PSRAM — see the plan §6). Track A only needs the FPS figure.
