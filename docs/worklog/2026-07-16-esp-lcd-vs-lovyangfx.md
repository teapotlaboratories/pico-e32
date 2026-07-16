# 2026-07-16 — Trying esp_lcd i80 instead of LovyanGFX: why colour "broke", and a real comparison

**Goal:** settle [`DP-1`/`DP-2`](../hardware/pico-e32-display.md) empirically. The repo currently says two
incompatible things about the `esp_lcd` i80 driver on the correct rev-1 pins, and the LovyanGFX submodule
is a 132 MB cost for one panel. Does esp_lcd actually work? If the only problem is colour, is it fixable?

**Status:** ⏸️ **concluded — esp_lcd parked, LovyanGFX stays.** esp_lcd i80 is **1.5× faster** (590 vs
393 fps, real zero-copy DMA) but its **colour is broken on this board in a way byte-swapping does NOT fix**
(every bright fill → the same teal, swap-invariant). Root-causing it needs a logic analyzer or owner
board-wiring insight (`DP-2`). `DP-1` resolved: the "colour wrong" code comments were overclaims — esp_lcd
had never been tested on the correct pins before now.

> **Read this worklog as a story with a twist.** The middle section builds a clean source-derived theory —
> "it's just a byte swap" — and then **the hardware refutes it** (see *Result 2*). The refuted theory is
> kept, not deleted, because the reasoning was sound and the refutation is the finding.

---

## The contradiction, in three places

The claim that esp_lcd "gets colour wrong" is asserted in the code **three times**, and denied by the
worklog once — all in `main`:

| where | says |
|---|---|
| `components/ili9488/ili9488.cpp:5-9` | esp_lcd **was** retried on rev-1 pins, "renders geometry correctly but gets COLOUR wrong: red and yellow come out blue/green, only two distinguishable hues instead of four" |
| `components/ili9488/CMakeLists.txt:3-5` | same — "gets colour wrong … so it is rejected" |
| `docs/worklog/2026-07-16-panel-rev1-pinmap.md:86` | "**`esp_lcd` i80 is untested on the correct pins**" |
| `…pinmap.md:115` | lists "Consider retrying `esp_lcd` i80 on the correct pins" as a *next step* |

So either it was retried and failed on colour, or it was never retried. One is wrong. This session
resolves it on the bench.

## Root cause of the colour difference — a source theory (later REFUTED on hardware, see Result 2 below)

> ⚠️ Everything in this section is the hypothesis I formed from reading source **before** flashing. It is
> internally sound and it predicts the reported symptom — but the bench proved it **wrong** (the real bug is
> swap-*invariant*). Kept as the honest record of the reasoning; the correction is in Result 2.

**LovyanGFX byte-swaps every pixel in hardware, via the GPIO matrix. esp_lcd does not.** That is the whole
colour difference, and it is a one-line-of-config problem, not a limitation.

`components/LovyanGFX/src/lgfx/v1/platforms/esp32s3/Bus_Parallel16.cpp:160-166` — LovyanGFX's data-pin
routing deliberately **crosses the two bytes**:

```c
auto idx_base = LCD_DATA_OUT0_IDX;
for (size_t i = 0; i < 8; ++i) {
    LGFX_GPIO_MATRIX_OUT(pins[i]  , idx_base + i+8);   // D0..D7  -> LCD_DATA_OUT8..15  (HIGH byte)
    LGFX_GPIO_MATRIX_OUT(pins[i+8], idx_base + i  );   // D8..D15 -> LCD_DATA_OUT0..7   (LOW byte)
}
```

`esp_lcd_new_i80_bus` maps `data_gpio_nums[i] -> LCD_DATA_OUT<i>` straight through (no crossover). So the
**same pixel bytes in DMA reach the panel byte-swapped** between the two drivers.

The ILI9488 in 16-bit parallel takes RGB565 as `RRRRRGGG GGGBBBBB`. Swap the two bytes and the panel reads
the green/blue byte as red and vice versa — pure red `0xF800` becomes `0x00F8` (dark blue-green), pure blue
`0x001F` becomes `0x1F00` (green), and the four PICO-8 primaries collapse toward **two muddy hues**. That is
*exactly* the reported symptom. It is a byte-order mismatch, and it has three independent fixes:

1. `esp_lcd_panel_io_i80_config_t.flags.swap_color_bytes` — ESP-IDF's own knob, "Swap adjacent two color
   bytes" (`vendor/esp-idf/components/esp_lcd/include/esp_lcd_io_i80.h:59`).
2. reorder the `data_gpio_nums` array (swap the low/high 8) to replicate LovyanGFX's crossover, or
3. the software swap already in `ili9488_rgb565` (the `swap_color_bytes` field of *our* config).

The catch is that exactly **one net swap** must happen. Our board config sets `.swap_color_bytes = true`, so
`ili9488_rgb565` already swaps in software. Whether that lands correct on esp_lcd depends on whether esp_lcd
adds a second swap — which is precisely the toggle to sweep on hardware. There are only a handful of
combinations, and the L-pattern + camera settles it in 1–2 flashes.

**The prediction, therefore:** esp_lcd i80 was **never fundamentally broken**. The "two days / 288 fps into
nothing" was the wrong pin map (already established). The colour claim, *if it was ever real*, is one
byte-swap toggle away from correct — and per the bench doc every colour claim this rig has produced has been
withdrawn, so it may not even have been real. To be verified on hardware next.

## Two esp_lcd variants existed (neither is the atanisoft SPI story)

- **Hand-rolled i80** (`components/ili9488/ili9488.c` @ `e0a21cc`, before the LovyanGFX swap): direct
  `esp_lcd_new_i80_bus` + `esp_lcd_new_panel_io_i80` + a vendor ILI9488 init, COLMOD `0x55` (RGB565),
  `esp_lcd_panel_io_tx_color(0x2C,…)`. This is the one that hit "288 fps". Clean and complete.
- **atanisoft `esp_lcd_ili9488` component** (v1.1.1, MIT, still vendored under the host firmware's
  `managed_components/`): supports both COLMOD `0x55` (16-bit) and `0x66` (18-bit RGB666). In 18-bit mode it
  expands RGB565→RGB666 in software (`esp_lcd_ili9488.c:233-251`) — **3 bytes/pixel, slow, and a different
  colour path entirely**. Its comment "16-bit color does not work via SPI interface" is an *SPI* caveat; on
  our *parallel* bus 16-bit is fine. Default MADCTL is `MX|BGR` (`:400`) — different again from LovyanGFX.

The hand-rolled variant is the better base for a fair retry: it is 16-bit, parallel-native, and already in
our git history.

## Implemented: a second backend, selectable at build time

`components/ili9488/ili9488_esp_lcd.c` (hand-rolled esp_lcd i80, based on `e0a21cc`), picked via
`components/ili9488/CMakeLists.txt`'s `ILI9488_BACKEND` cache var, forwarded by the Makefile as
`ILI9488_BACKEND=esp_lcd`. Both drivers satisfy the same `ili9488.h`; neither file is deleted
(least-destructive). LovyanGFX stays the default. Builds clean for both `display-test` and `host`.

## Result 1 — esp_lcd is DRAMATICALLY faster: 590 fps vs LovyanGFX's 393 (measured)

Same Track-A harness, same 40 MHz, same 256×256:

| | blit-only | frame time | % of 610.4 ceiling | end-to-end |
|---|---|---|---|---|
| **LovyanGFX** | 393.0 fps | 2.54 ms | 64.4% | 210.6 fps |
| **esp_lcd i80** | **590.2 fps** | **1.69 ms** | **96.7%** | **256.5 fps** |

This is the `DP-3` finding made real. LovyanGFX's `pushImage` uses the non-DMA path (512 memcpy'd
256-byte chunks/frame); esp_lcd is **true zero-copy DMA** — one descriptor chain, the CPU sleeps through
the transfer. The comparison workflow predicted ~600 fps from source (`esp_lcd_panel_io_i80.c`: pointer
mounted straight onto the GDMA link, ISR fires on the real transaction-done event, semaphore blocks the
blit until the last pixel is clocked); the bench delivered 590.2, at 96.7% of the bus ceiling. Blocking is
honest — the fps loop measures completed transfers, same bar Track A meets. Clock is a wash (esp_lcd also
lands 40.000 MHz exactly, 160 MHz ÷ 2 ÷ 2).

**So on speed, esp_lcd wins by ~1.5×.** If throughput ever mattered (it doesn't for Gate #1 — LovyanGFX
already passes 13× over), this is the path.

## Result 2 — but esp_lcd's COLOUR is broken on this board, and NOT as a byte swap

Every configuration tried renders colour wrong. Judged on the L-pattern (static) and solid R/G/B fills:

| config | result (camera) |
|---|---|
| straight pins, app pre-swap (= the original `e0a21cc`), `flags.swap_color_bytes=1` | shredded, all-teal |
| straight pins, `flags.swap_color_bytes=1`, MADCTL 0x98 | shredded shapes, teal |
| **data-pin crossover** (mimic LovyanGFX), app pre-swap | **worse** — uniform teal, no shapes (crossing the pins scrambles the 8-bit init COMMANDS that share the bus, so the panel barely configures) |
| straight pins, **native** RGB565 (no swap), no flag | recognisable shapes, teal; **solid R=G=B fill = same teal** |
| straight pins, **app pre-swap** (swapped), no flag | **identical to native** — solid R=G=B fill = same teal |

**The decisive, and baffling, observation:** toggling the app's byte pre-swap (native ↔ swapped) changes
**nothing** on the glass — solid red, green and blue fills all render the **same teal**, either way. A
byte-order bug *cannot* do that: swapping the two bytes of `0xF800` vs `0x00F8` must change the hue if the
panel is decoding the bytes at all. Brightness and structure survive (the white-on-black L is legible;
black stays black), but **hue collapses to a single teal regardless of the pixel value.** For contrast,
LovyanGFX on the identical harness renders the 16 palette bars in distinct, varied hues (measured
RGB varies bar to bar); esp_lcd renders them all teal.

**So the repo's old "red/yellow come out blue/green" is directionally right (colour IS wrong on esp_lcd)
but the mechanism is deeper than the byte-order story I traced from source.** The workflow's bit-level
prediction — that an odd number of swaps turns the PICO-8 warm palette toward blue — is sound *as a
prediction*, but it is not what the hardware does: the hardware is swap-*invariant*, which no amount of
byte-swapping fixes.

### Working theory (unconfirmed — needs a logic analyzer)

LovyanGFX drives this panel correctly using a **GPIO-matrix data-pin crossover** (D0-7 ↔ D8-15,
`Bus_Parallel16.cpp:160-166`) that it applies to *data* while still getting *commands* out — because it
packs commands with the crossover in mind. esp_lcd has no such split: its data pins are fixed at bus
creation and shared by commands. When I replicate the crossover by reordering `data_gpio_nums`, the init
commands scramble and the panel won't configure; when I leave the pins straight, the colour bits don't
land. The board's actual DB-pin wiring is unknown (not on the silkscreen), and every theory here is a
guess about a bus I can't see. **Resolving it needs a logic analyzer on D0-D15 + WR + DC**, or the owner's
knowledge of how this rev-1 board wires the panel data bus — the same class of board-specific fact that
cracked the pin map.

### Correcting the record (DP-1 resolved, honestly)

The worklog side of the contradiction was right: esp_lcd was **never actually tested on the correct pins**
before this session. The code comments (`ili9488.cpp:5-9`, `CMakeLists.txt`) that stated "retried … gets
colour wrong" as a *measured* result were overclaiming — the colour failure is real, but it was inherited
from wrong-pin runs and a source prediction, not a bench test, until now. This session is the first real
test, and it both **confirms colour is broken** and shows the stated *reason* (a byte swap) is **wrong**.

## DP-2 recommendation: stay on LovyanGFX (for now)

esp_lcd's 1.5× speed is real and appealing, and the DMA architecture is cleaner. But it does not render
correct colour on this board, the cause is not a config toggle, and Gate #1 already passes 13× over on
LovyanGFX — so there is **no pressure** to switch and a **real, unsolved blocker** against it. Keep
LovyanGFX as the default; keep the esp_lcd backend in-tree (it builds, it's fast, it's the substrate for a
logic-analyzer debugging session). Revisit if (a) a future heavy cart needs the throughput, or (b) the
owner has board-wiring insight that explains the swap-invariant teal.

## Board left as

Restored to the known-good **LovyanGFX** `display-test` build on `/dev/ttyUSB1` — palette bars, distinct
colours, upright. The esp_lcd backend is in the tree but not default.
