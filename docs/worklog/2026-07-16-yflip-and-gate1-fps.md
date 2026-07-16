# 2026-07-16 — Fixing the Y-flip, and measuring Gate #1 for the first time honestly

Picks up from [the pin-map worklog](2026-07-16-panel-rev1-pinmap.md), which left exactly two things open:

1. **The panel renders Y-flipped.** Shape-verified with the asymmetric L-pattern cart in
   `firmware/pico-e32-host`, so it is real and not a camera artifact. LovyanGFX owns orientation now;
   `.madctl` in `board_pins.h` is dead config.
2. **Gate #1's fps is VOID.** The old *288 fps* was DMA into unconnected pins, on `esp_lcd` at 20 MHz.
   The bus is now LovyanGFX at 40 MHz actually driving a panel. The real number has never been taken.

**Status:** 🔨 in progress.

---

## Bench state at session start

| | |
|---|---|
| board under test | `/dev/ttyUSB1` — ESP32-S3 (QFN56) rev v0.1, **Embedded PSRAM 2MB** → confirms N16R2 / rev 1 |
| bench camera | `/dev/ttyUSB0` — ESP32-D0WDQ6-V3, serving `http://192.168.7.135` |
| flashed | `pico_e32_display_test`, built `Jul 16 2026 06:13:00`, ELF SHA256 `4fc2f7bf4…` — i.e. **already** the LovyanGFX / rev-1-pins build |
| tree | clean at `5e2dae7` |

Identified with, per the bench doc (the `/dev/ttyUSB*` numbering is not stable — it must be checked,
not assumed):

```sh
esptool.py -p /dev/ttyUSB0 flash_id   # -> Chip is ESP32-D0WDQ6-V3   (bench camera)
esptool.py -p /dev/ttyUSB1 flash_id   # -> Chip is ESP32-S3 (QFN56)  (board under test)
```

> ⚠️ **`esptool.py flash_id` power-cycles the target.** That resets the bench camera's sensor
> registers, so the tuned `awb=0&exp=600&gain=0&sat=2` settings are **lost** and must be re-applied on
> the next `/capture`. They persist in the sensor, not in flash. Probing the camera to identify it and
> then trusting a bare `/capture` would silently measure with auto-exposure — the exact thing the bench
> doc says destroys colour judgement.

Baseline capture (`/capture?awb=0&exp=600&gain=0&sat=2`, rotated 90° CW per the rig's geometry): the
16 palette bars, centred, sharp, LCD pixel grid visible. Colours are palette-rotated rather than in
canonical PICO-8 order — expected: `main.c`'s measurement loop rotates the palette every 16 frames and
leaves it wherever it stopped. The rig is behaving exactly as the bench doc describes.

## Finding 1 — the fps number was already there, unread: **370.3 fps**

The flashed build already prints it. Nobody had read the UART after the pin fix:

```
I (792) ili9488: ILI9488 up via LovyanGFX (320x480, 40000000 Hz, cs=37, dlen_16bit)
I (802) trackA: palette bars drawn at (32,112) 256x256
I (5822) trackA: ==== Gate #1: 1856 frames in 5.01s = 370.3 fps (256x256 blit) ====
I (5822) trackA: PASS if >= 30 fps.  (bus ceiling ~305 fps @16-bit/20MHz)
```

**This number is not yet trustworthy, and the last line is why.** `~305 fps @16-bit/20MHz` is stale
text from the `esp_lcd` era — the bus is 40 MHz now. Taken at face value the log says we exceeded the
bus ceiling by 21%, which would be impossible. Both numbers needed re-deriving before either was
quoted; that is done below, and the contradiction dissolves.

### Correction — my first reading of this was wrong twice, in opposite directions

Recorded because both errors are instructive.

**Error 1: "370 > 305, so something is broken."** No — `305` is a **20 MHz** number. At the configured
40 MHz the 256×256 ceiling is **610.4 fps** (derived below). 370.3 is 60.7% of it. There was never a
contradiction, only a stale constant sitting next to a live measurement — which is *exactly* how the
plan's original, correct `≈130 fps` figure got "corrected" into the wrong one (see below).

**Error 2: my defence of the number was unsound, even though its conclusion was right.** I argued a
5-second average can't be inflated by async DMA because "you cannot enqueue faster than the DMA drains;
the queue backpressures." **There is no queue.** `Bus_Parallel16::writeBytes` is a *1-deep pipeline*
with a spin-wait (`Bus_Parallel16.cpp:415`), and the real guarantee is a **hard drain** in
`endWrite → endTransaction → _bus->wait()` (`Panel_LCD.cpp:87` → `Bus_Parallel16.cpp:211-216`). The
distinction matters: a genuinely *unbounded* async queue **could** have inflated a 5-second average, so
the argument as written wasn't safe. The source is what makes it safe, not the arithmetic of averages.
Getting the right answer from bad reasoning is how the last two days happened.

## Finding 2 — the measurement loop trips the task watchdog

Not previously recorded. The Gate #1 loop never yields, so `IDLE0` is starved for the whole 5 s:

```
E (5782) task_wdt: Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:
E (5782) task_wdt:  - IDLE0 (CPU 0)
E (5782) task_wdt: Tasks currently running:
E (5782) task_wdt: CPU 0: main
E (5782) task_wdt: CPU 1: IDLE1
```

It fires at t≈5.78 s — **inside** the 5.01 s measurement window (which starts after init at t≈0.8 s).
Printing the WDT backtrace over a 115200 UART is not free, and it lands in the timed region, so it can
only have **depressed** 370.3 fps. The true rate is that or higher. Fix the yield before quoting a
number.

## Finding 3 — the Y-flip, reproduced and measured (not eyeballed)

Flashed `pico-e32-host` (the L-pattern cart) and captured. Rather than judge the frame by eye — the
failure mode this rig has already caused twice — the capture was **blob-detected and mapped back into
cart coordinates**. Scale: the L-bar spans the full 128-px cart width across 928 capture px →
**7.25 capture px per cart px**, which anchors the mapping `capture_y = 370 + cart_y × 7.25`.

| feature | drawn at (cart) | **observed at (cart)** | observed mean RGB | reads as |
|---|---|---|---|---|
| red `8` marker | TL (20,20) | **BL** (17.7–33.5, 84.7–102) | (85,42,37) | dark red |
| green `11` marker | TR (92,20) | **BR** (92–110.5, 89.5–107) | (31,145,26) | green (red=31) |
| blue `12` marker | BL (20,92) | **TL** (21–38, 11–28) | (8,84,157) | blue |
| yellow `10` marker | BR (92,92) | **TR** (95.7–113, 12–30) | (82,126,22) | yellow-green (red=82) |
| L bar, full width | **top** y=0–7 | **bottom** | (63,131,110) | white `7` + cast |
| left bar | x=0–7, **top** half | x=0–7, **bottom** half | — | white `7` + cast |
| stub | **right**, x=64–120 | **right**, x=64–126 | (64,153,141) | white `7` + cast |

**Verdict: a pure vertical mirror. Every top↔bottom pair swaps; nothing left↔right does.**

The two greens are separated by their **red channel** (31 vs 82), not by absolute hue — per the bench
doc's rule, relative colour only. The yellower one sits where `green` was drawn and vice versa, which
is the Y-flip again, independently of the L-shape.

**It is specifically not a 180° rotation**, and the stub is what proves it: under 180° the stub
(drawn right of centre) would appear **left** of centre and TL would show *yellow*. The stub is on the
right and TL is blue. The stub is also useless for the Y question — `rectfill(64,62,120,65)` is
Y-symmetric about the 128-px centre (`127-65..127-62` = `62..65`), so it maps onto itself under a
Y-flip. It earns its keep on the X axis only.

### The one assumption this rests on, stated honestly

All of the above is **conditional on the bench doc's claim that the camera is mounted 90° CW** (LEFT of
frame = TOP of panel); the analysis applies that correction before judging. If that mounting note were
wrong by 180°, the identical capture would instead mean an **X-flip**. Nothing in the frame settles
this — the rig cannot see its own mounting, and the 256×256 blit is centred on the panel, so even the
panel's borders offer no independent anchor.

It does not need to be settled analytically: **the fix loop settles it.** Apply a Y-flip and capture.
Upright → the diagnosis was right. 180°-rotated → the mount note is off and the real defect is an
X-flip. Two iterations, worst case, and the answer is empirical rather than argued.

*(It settled on the first iteration — see Finding 5. The panel came up upright, which retro-confirms
the bench doc's 90° CW mounting note as a side effect.)*

## Finding 4 — the fix: `offset_rotation = 4`, and why it is the only candidate

LovyanGFX owns orientation, and its rotation arithmetic is **not** the obvious `(r + offset) & 7`.
`Panel_LCD::setRotation` (`components/LovyanGFX/src/lgfx/v1/panel/Panel_LCD.cpp:130`):

```c
_internal_rotation = ((r + _cfg.offset_rotation) & 3) | ((r & 4) ^ (_cfg.offset_rotation & 4));
```

Bit 2 is a **mirror flag that is XORed in**, so it can never carry into the rotation bits. With the
app's rotation at 0, `offset_rotation = 4` → `_internal_rotation = 4`. The MADCTL table
(`Panel_LCD.hpp:85-99`; `Panel_ILI9488` does **not** override `getMadCtl` — the override at
`Panel_ILI948x.hpp:87` belongs to the *sibling* `Panel_ILI9481`, which is easy to misread):

| `_internal_rotation` | MADCTL (+BGR) | MV (axis swap) | MX | MY |
|---|---|---|---|---|
| 0 | **0x08** ← was | 0 | 0 | 0 |
| 2 | 0xDC | 0 | 1 | **1** |
| **4** | **0x98** ← now | 0 | 0 | **1** |
| 6 | 0x4C | 0 | **1** | 0 |

The MADCTL bit semantics are not folklore — they are documented in-tree, at
`vendor/esp-idf/components/esp_lcd/include/esp_lcd_panel_commands.h:51-56`:

| bit | | meaning |
|---|---|---|
| `MY` | `1<<7` | Row address order — 0: top to bottom, **1: bottom to top** ← the mirror |
| `ML` | `1<<4` | Line address order — 0: refresh top to bottom, 1: refresh bottom to top |
| `MX` | `1<<6` | Column address order — 0: left to right, 1: right to left |
| `MV` | `1<<5` | Row/Column exchange — 0: normal, 1: reverse (the axis swap) |

So rotation 4's `MY|ML` is exactly a vertical mirror (`MY`) with the panel's **refresh** direction
brought along to match (`ML`), which is why LovyanGFX pairs them in every table entry — it keeps scanout
travelling the same way as addressing, avoiding a tear. *(That header carries its own caveat at `:48-50`
— these bit positions are conventional, not standardised, so check the panel's datasheet. It corroborates
LovyanGFX's table rather than independently proving it; the hardware result below is what settles it.)*

**4 is the only value in 0–7 that inverts Y while leaving MX and MV clear.** 1/3/5/7 all set `MV` and
would swap the panel to 480×320; 2 mirrors X as well. Geometry is provably untouched: `4 & 1 == 0` so
no `ox/oy`/`pw/ph` swap (`Panel_LCD.cpp:138`); `4 & 2 == 0` so `_colstart = 0` (`:146`); and
`_rowstart = mh - (ph + oy) = 480 - 480 = **0**` (`:149`) — which lands on zero **only because**
`memory_height == panel_height` and `offset_y == 0`, both true here. So `pushImage(32,112,256,256,…)`
addresses the identical pixels, just scanned bottom-up. Cost: one MADCTL byte at init, **zero per frame**.

**The dead `.madctl = 0x48` disagreed with this — and carries no weight.** It named `MX|BGR`; the fix
needs `MY`. But `0x48` was never applied by *any* driver, and the value actually going to the panel was
`0x08`. It was set during the era when the bus clocked into unconnected pins, so it was never validated
against a lit panel either. It is not a second opinion; it is noise that looked like config.

## Finding 5 — ✅ **FIXED AND VERIFIED UPRIGHT**, first try

`mirror_y = true` → `offset_rotation = 4`. Flashed `pico-e32-host`, captured, blob-detected:

|  | L bar position | TL | TR | BL | BR | verdict |
|---|---|---|---|---|---|---|
| **before** | **BOTTOM** (arm widths 56 vs 611 px) | blue | yellow | red | green | pure Y-flip |
| **after** | **TOP** (arm widths 890 vs 55 px) | **red** | **green** | **blue** | **yellow** | ✅ **UPRIGHT** |

Every marker lands where the cart draws it; the L is on top+left with the notch top-left; the stub
points right at mid-height. **Pass, stated against the frame.**

**It came up upright, not 180°-rotated — so the assumption above holds.** The bench doc's "camera is
mounted 90° CW" note is correct, and the defect really was a Y-flip rather than an X-flip. Had the mount
note been wrong, this same change would have produced a 180° image.

### Where the fix lives, and why that matters more than the value

In **`boards/makerfabs-ili9488-r1/board_pins.h`**, as `.mirror_y = true`, replacing the dead
`.madctl = 0x48` — *not* in the driver.

The glass on this board is mounted upside-down relative to the controller's native scan direction. That
is a fact about **this board**, not about the ILI9488 and not about LovyanGFX, so it belongs in the file
that calls itself *"THE ONE DEFINITION"* for board facts. The driver now just asks
`c->mirror_y ? 4 : 0`. A different ILI9488 board provides its own answer, and `ili9488_config_t` stays
free of LovyanGFX's rotation encoding — which matters, because retrying `esp_lcd` on the correct pins is
still an open option and `mirror_y` survives that swap where an `offset_rotation` byte would not.

**Corroboration that this is a board fact and not our bug:** the rev-1 source of record
(`references/makerfabs-parallel-tft-lvgl-lgfx/main/LGFX_MakerFabs_Parallel_S3.hpp:106`) also sets
`offset_rotation = 0` — our orientation config was byte-identical to the vendor's. But **the vendor never
ran rotation 0**: their demo calls `lcd.setRotation(1)` (`main/main.cpp:134`, landscape 480×320). So
`_internal_rotation = 0` on this board was untested territory upstream, and the reference matching us was
never evidence that rotation 0 renders correctly. It is the same trap as the pin map, one layer in: the
vendor's file is only authoritative for **the configuration the vendor actually ran**.

---

## Gate #1 — measured honestly

### The ceiling, re-derived (and the old one was never wrong)

The ILI9488's notorious RGB666/18-bit coercion — 2 bus writes per pixel — is **SPI-only**. LovyanGFX
applies it only when `busType() == bus_spi` (`Panel_ILI948x.hpp:169-179`); on `Bus_Parallel16` the panel
gets `COLMOD (0x3A) = 0x55` = RGB565/16bpp (`Panel_LCD.hpp:81,137`), and `dlen_16bit` puts 16 bits on the
wire per WR strobe. **One bus cycle per pixel.**

```
256 × 256                  = 65,536 px = 65,536 WR cycles
40 MHz × 2 B/cycle         = 80 MB/s
65,536 ÷ 40e6              = 1.6384 ms  ->  CEILING = 610.4 fps
```

40 MHz is exact, not nominal: `calcClockDiv` picks 240 MHz ÷ 2 ÷ 3 = **40.000 MHz**, no fractional
divider (`Bus_Parallel16.cpp:130`, `common.cpp:285-319`). Note `lcd_clk_sel = 2` selects the **240 MHz**
source — the `LCD_CLK_SRC_PLL160M` at `Bus_Parallel16.cpp:99` is overwritten in `beginTransaction`.

**A correction to the record, in the project's favour.** The Gate #1 worklog declared the plan's
`≈130 fps theoretical @16-bit/20MHz` "wrong" and replaced it with `~305`. **The 130 was right.** It is a
full-panel 320×480 figure: `153,600 ÷ 20e6 = 130.2 fps` ✓, and its companion "≈32 fps at 8-bit/10 MHz"
checks out too (`307,200 B ÷ 10 MB/s = 32.6`). The worklog compared a **320×480 ceiling** against a
**256×256 measurement**, called the ceiling wrong, and overwrote it — which is how `main.c` came to print
a stale, mismatched `~305 fps @16-bit/20MHz` for two days. A correct number was destroyed by comparing it
against a different quantity. The harness now **derives** the ceiling from `cfg.pclk_hz` instead of
quoting a constant, and logs an error if a measurement ever exceeds it.

### The numbers

Harness reworked: fixed frame count (auditable, nothing in flight at the edges), 32 untimed warm-up
frames, min/max/mean frame time (the gate says *sustained*; a mean hides a stall), a yield between
phases, and **two numbers instead of one**.

```
I (802)  trackA: bus ceiling 610.4 fps  (16-bit @ 40.0 MHz, 256x256 RGB565, 1 cycle/px)
I (2422) trackA: blit-only   600 frames in 1526.638 ms ->  393.0 fps  (frame 2.54 ms mean, 2.54 min, 2.55 max)  64.4% of ceiling
I (4002) trackA: end-to-end  300 frames in 1424.641 ms ->  210.6 fps  (frame 4.75 ms mean, 4.75 min, 4.75 max)  34.5% of ceiling
```

| | fps | frame time | vs 30 fps gate | vs ceiling |
|---|---|---|---|---|
| **blit-only** (A2's "flush loop": push a pre-built 256×256) | **393.0** | 2.54 ms | **13.1×** | 64.4% |
| **end-to-end** (palette-expand + 2× scale + blit, every frame) | **210.6** | 4.75 ms | **7.0×** | 34.5% |

## 🚦 **GATE #1: PASS.** Both numbers clear ≥ 30 fps by 7–13×, and the display path is not the bottleneck.

**Why two numbers, not one.** The old loop rebuilt the scaled buffer on **1 frame in 16** and reported
the blend as "256x256 blit" — neither the flush rate nor the per-frame cost a real cart pays. The plan is
genuinely ambiguous here (A2 says *"time the sustained flush loop"*, the gate says *"the 256×256
**scaled** full-refresh"*), so the harness now answers both readings and lets the reader pick. Reporting a
single number is precisely the habit that let "288 fps" stand for "the display works".

Spread is essentially zero — 2.54 min / 2.55 max over 600 frames, and 4.75/4.75 over 300 — and eight
consecutive passes reproduce to the 0.1 fps. This is a genuinely sustained rate, not an average hiding
stalls. The expand+scale costs `4.75 − 2.54 = **2.21 ms/frame**`, i.e. the CPU-side palette work is
comparable to the entire bus transfer.

**The watchdog fix moved the number, as predicted.** 370.3 → **393.0 fps** (+6.1%) once the yield kept the
TWDT backtrace out of the timed window. Finding 2 said "the true rate is that or higher"; it was higher.

### The evidence that this number is not another 288

370 or 393 fps proves the **ESP32's LCD_CAM finished strobing WR**. It proves nothing about the panel —
bus timing is *identical* whether the pins reach an ILI9488 or nothing at all. That gap is unclosable by
timing, and it is exactly the gap the 288 fps fell through. Only the camera closes it.

**`pushImage` does fully drain — verified in source, and the mechanism is not the obvious one.**
`Bus_Parallel16::writeBytes` is *pipelined, not blocking*: it spins on the **previous** transfer
(`Bus_Parallel16.cpp:415`), sets `LCD_CAM_LCD_START` and returns with DMA in flight. The drain happens a
level up, because each `ili9488_blit` is its **own** transaction, so the unnested `endWrite()` →
`endTransaction()` → `_bus->wait()` (`Panel_LCD.cpp:87` → `Bus_Parallel16.cpp:211-216`) spins to
completion. **The loop measures completed transfers.** This is now documented at the `ili9488_blit`
declaration, because it is fragile: wrapping blits in an outer `startWrite()/endWrite()` — the standard
LovyanGFX efficiency idiom — would migrate the drain outward and silently turn any fps loop into a
measurement of the queueing rate.

**Camera evidence: the panel is visibly live *during* the timed window.** Four captures, 1 s apart, while
the measurement loop ran:

- **Content changes between every pair.** Mean |difference| across the full frame: **12.1 – 24.3** levels.
  The panel region's mean RGB swings R 31.6→45.5, G 56.9→91.1. Far above sensor noise.
- **The two phases are visually distinguishable, and they alternate.** `live-1` and `live-3` show 16
  clean stripes — captured during **blit-only**, where the content is static. `live-2` and `live-4` show a
  torn patchwork — captured during **end-to-end**, where the palette advances every frame. The tear bands
  run along **panel rows** (the 90° mount maps panel-y to frame-x): GRAM is being rewritten at 210 fps
  while the panel scans its own glass at ~60 Hz (`Panel_ILI948x.hpp:209`: `FRMCTR1 = 0xA0`, 60 Hz), so
  different rows hold different palette generations at the instant of exposure. 2 clean / 2 torn,
  alternating, matches the 1.53 s / 1.42 s phase split.

> **Honesty note — a measurement I attempted and threw away.** I tried to *count* tear bands
> programmatically to quantify the write rate. The detector is unreliable and its numbers are **not
> reported**: at this focus the **LCD pixel grid is resolvable** (which is the rig working as designed —
> see the bench doc's focus curve), and that high-frequency pattern registers as "steps" along the same
> axis as a tear. Smoothing above the grid pitch then picked up the panel edge instead, and it flagged
> `live-3` — plainly clean to the eye — as torn. The tearing is real and visible; my count of it was not
> trustworthy, so it stays a **qualitative** observation. Per the bench doc: judge structure from a
> capture. Structure says live; an invented band-count would have been this session's "double byte-swap".

**What is still NOT proven, and cannot be by anything above:** that every pixel latches *cleanly*. The
panel is clocked at **40 MHz (25 ns WR cycle) against a datasheet 8080-II minimum of 50 ns (20 MHz)** —
2× its rating. That value comes from the vendor's own rev-1 config
(`LGFX_MakerFabs_Parallel_S3.hpp:76`: `freq_write = 40000000`), and it renders, which is the only evidence
behind it; there is no timing-margin analysis. Marginal latching would show as sparkle or dropped pixels,
**not** as a wrong fps. Judge from the frame, never the counter.

### One thing the number must never be read as

**393 fps is a GRAM-write rate, not photons per second.** The panel scans its own glass at ~60 Hz, so
nothing above ~60 is ever visible — to the eye or the camera. The tearing above *is* that fact made
visible. This does not weaken Gate #1, which asks whether the blit path can sustain 30 fps of work (it
can, 13× over), but "the screen updates 393 times a second" would be false.

---

## What changed

| file | change |
|---|---|
| `boards/makerfabs-ili9488-r1/board_pins.h` | `.madctl = 0x48` (dead) → **`.mirror_y = true`** (the board fact), + why |
| `components/ili9488/ili9488.h` | `madctl` → `mirror_y` in `ili9488_config_t`; **every doc comment corrected** — the header still described the deleted `esp_lcd` implementation ("driver over … esp_lcd i80", "delegated to `atanisoft/esp_lcd_ili9488`", "use `esp_lcd_panel_mirror`" — there is no esp_lcd handle in this driver); documented `swap_color_bytes`'s real meaning and the drain's fragility |
| `components/ili9488/ili9488.cpp` | `p.offset_rotation = c->mirror_y ? 4 : 0` + the rotation arithmetic explained |
| `firmware/pico-e32-display-test/main/main.c` | ceiling **derived** from `cfg.pclk_hz`; blit-only **and** end-to-end reported; fixed frame count; warm-up; min/max; yield (TWDT); over-ceiling assert; killed the comment pointing at `.madctl` |
| `docs/hardware/pico-e32-display.md` | **new** — display area doc + backlog `DP-1`…`DP-7` |

## Found on the way — filed, not fixed

All in [`docs/hardware/pico-e32-display.md`](../hardware/pico-e32-display.md). Deliberately **not**
bundled into this change: Gate #1 passes 13× over without any of them, and the Y-flip fix should be
reviewable on its own.

- **`DP-1` — the repo contradicts itself about `esp_lcd`, in `main`, right now.** `ili9488.cpp:5-9`
  says esp_lcd i80 **was** retried on the correct rev-1 pins and "gets COLOUR wrong: red and yellow
  come out blue/green, only two distinguishable hues instead of four". The pin-map worklog says it
  **was never tried** (`:86`) and lists "consider retrying" as a next step (`:115`). Committed **36
  seconds apart** (`66b3958`, `080d68f`). One is false and nothing marks which. It also gates the
  132 MB-LovyanGFX-submodule decision (`DP-2`). Given this rig's record, the *colour* claim is the one
  I'd doubt first — but it was never written up, so I am flagging it, not overruling it.
- **`DP-3` — blits never use DMA.** `pushImage`'s `use_dma` defaults to **false**, so each frame is 512
  serialised 256-byte chunks + 128 KB of memcpy instead of one descriptor chain. Explains most of the
  393 → 610 gap, and the per-chunk overhead arithmetic (~2.07 µs) closes to the measurement exactly.
  Has a real trap: DMA reads the caller's buffer, so it and the harness's rebuild-in-place are
  individually safe and **jointly a tearing race**.
- **`DP-4` — the checksum's coverage is oversold**, and `swap_color_bytes` can desync host from driver
  *without changing the hash*. The hash covers the indexed framebuffer only — everything from palette
  expansion to the wire, i.e. exactly where the two-day bug lived, is outside it.
- **`DP-5` — the negative control.** Deliberately misconfigure WR/CS, confirm fps is unchanged while
  the camera goes blank. Gate #1's own worklog *predicted the 288 fps failure in writing* and it was
  written off as hypothetical. Running it once makes it a fact.
- **`DP-6`** `ili9488_fill` is slow, redundant (LovyanGFX already clears at init) and has a latent OOB.
  **`DP-7`** `bus_shared = true` is inert but its comment is false and it diverges from the source of record.

**Not filed — a repo-hygiene papercut for the owner to rule on.** `firmware/pico-e32-host/host/host` is a
**compiled 295 KB binary tracked in git**, alongside 10 generated `frames/*.raw` dumps and a `.png`. Simply
running the host build dirties the tree — I hit it this session and had to `git checkout` it back.
`.gitignore:15-16` already ignores the *equivalent* artifact for the other app
(`firmware/pico-e32-luabench/host/bench`), so the pattern exists and just was not extended. Left alone
deliberately: `git rm`-ing tracked files is destructive, and `frame_009.png` may well be intentional
reference evidence rather than an artifact. Owner's call.

## Next

- **Re-measure Gate #3.** Its 161.5 fps predates the Y-flip fix and has the same never-yielding loop, so
  a TWDT backtrace lands inside its 1-second reporting windows (~1 in 5 reports depressed).
- `DP-1` first — it is cheap and it is a live contradiction in the record.
- Extend the Gate #2 bench to levels 16–30 ([host-graphics worklog](2026-07-15-host-graphics.md)).

## Board left as

`pico-e32-display-test` @ `makerfabs-ili9488-r1` on `/dev/ttyUSB1` — the Gate #1 harness, looping
blit-only/end-to-end forever and printing both. Panel upright, animating, no watchdog. Commands used
throughout:

```sh
make build APP=pico-e32-display-test BOARD=makerfabs-ili9488-r1
make flash APP=pico-e32-display-test BOARD=makerfabs-ili9488-r1 PORT=/dev/ttyUSB1
curl -o /tmp/f.jpg 'http://192.168.7.135/capture?awb=0&exp=600&gain=0&sat=2'   # then rotate 90 CW
```
