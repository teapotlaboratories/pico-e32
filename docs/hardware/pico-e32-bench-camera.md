# Bench camera (M5Stack Timer Camera F) — hardware-in-the-loop capture

The bench camera is equipment, not handheld firmware: it is aimed at the panel under test
and serves a still JPEG over HTTP so display changes can be verified by *looking at what
actually renders*, per [`.ai/AGENTS.md` → Verifying changes](../../.ai/AGENTS.md#verifying-changes).

- **Firmware:** [`firmware/pico-e32-bench-cam`](../../firmware/pico-e32-bench-cam) (target `esp32`, board `m5stack-timer-cam`)
- **Capture tool:** [`tools/capture_frame.sh`](../../tools/capture_frame.sh)
- **Frames land in:** `/tmp/pico-e32-captures/<timestamp>[-label].jpg` — **never the repo**
  (per [`.ai/AGENTS.md`](../../.ai/AGENTS.md) → *Hardware & flashing notes*). Override with
  `CAPTURE_DIR=` only for a frame being kept as evidence.

## Boards on the bench

| role | board | chip |
|------|-------|------|
| **device under test** | Makerfabs ILI9488 (ESP32-S3 N16R2) | ESP32-S3 |
| **bench camera** | M5Stack Timer Camera F (OV3660, fisheye) | ESP32-D0WDQ6-V3 |

> Both boards enumerate as `/dev/ttyUSB*` and **the numbering is not stable** — it depends on
> plug order. Confirm which is which before flashing, so you don't flash the camera firmware
> onto the board under test:
> ```sh
> esptool.py -p /dev/ttyUSB0 flash_id | grep 'Chip is'
> # ESP32-D0WDQ6-V3 -> the bench camera
> # ESP32-S3        -> the board under test
> ```

## One-time setup

**WiFi credentials are never stored in the tree** — pass them on the build command, where they
reach the firmware as compile-time macros:

```sh
make flash APP=pico-e32-bench-cam BOARD=m5stack-timer-cam PORT=/dev/ttyUSB0 \
     WIFI_SSID='my ssid' WIFI_PASS='my pass'
```
The build fails with an `#error` if they are missing.
On boot it logs its address over UART:
```
I (…) camera: Detected OV3660 camera
I (…) bench-cam: ==== bench-cam ready:  http://192.168.x.y/capture  ====
```

Then point the capture tool at it:
```sh
cp tools/bench_cam.env.example tools/bench_cam.env   # set BENCH_CAM_HOST=<ip>
```

### Gotchas that cost real time

- **Baud:** this board's FTDI bridge cannot sustain idf.py's default 460800 — the chip syncs
  at 115200 and then dies on the baud switch with `No serial data received`. Pinned to 115200
  in [`boards/m5stack-timer-cam/board.mk`](../../boards/m5stack-timer-cam/board.mk); no flag needed.
- **Port permissions:** the serial node is owned by `root:dialout`, so a one-off `chmod` does
  not survive a replug. Join the group once — `sudo usermod -aG dialout $USER` — then log out
  and back in (`id -nG` should list `dialout`). For a shell that predates the change, prefix
  with `sg dialout -c '...'` rather than reaching for `chmod`.
- **Identifying the boards resets the camera's tuning.** The `esptool.py flash_id` above — the very
  command this doc tells you to run to tell the two boards apart — **power-cycles the target**. The
  measuring settings below live in the **sensor's registers, not in flash**, so probing the camera
  silently reverts it to auto-exposure/auto-white-balance. A bare `/capture` afterwards then quietly
  measures with exactly the AWB this rig exists to avoid. **Re-apply the settings on the first capture
  of any session** (they persist until the next reset):
  ```sh
  curl -o frame.jpg 'http://<ip>/capture?awb=0&exp=600&gain=0&sat=2'
  ```
- **Lens film:** these ship with a protective film over the lens — captures come back black,
  and auto-exposure will *not* save you. Peel it before blaming the firmware.
- **A failed camera probe self-diagnoses:** if `esp_camera_init()` fails, the firmware scans the
  SCCB bus, tests the lines for pull-ups, sweeps every plausible pin pair, and tries each known
  board pin map — the log distinguishes a pin-map error from dead/unpowered hardware.

## Capturing

```sh
tools/capture_frame.sh                 # -> /tmp/pico-e32-captures/20260715-101500.jpg
tools/capture_frame.sh gate1-bars      # -> /tmp/pico-e32-captures/20260715-101500-gate1-bars.jpg
```
It prints the path it wrote. The endpoint drops one frame before returning, so you always
get a *current* image rather than a stale buffered one.

## The loop (display changes)

```sh
make flash APP=pico-e32-display-test BOARD=makerfabs-ili9488-r1 PORT=/dev/ttyUSB1
sleep 2 && tools/capture_frame.sh gate1-bars
# inspect the frame; adjust madctl / swap_color_bytes; repeat
```
**Judge pass/fail against the captured frame** — not against "the draw call returned".

## What a capture can and cannot tell you

**The rule: judge shape and position from a capture. Treat colour as indicative only — act on it
only if a hue lands in a completely different family (red showing as blue).** Everything below is a
mistake this rig has already caused; the rule is what survives them.

- **The camera is not a colorimeter.** Some colour shift is present in *every* frame and is not a
  bug. An emissive LCD behind a fixed-white-balance sensor will cast — worse, `awb=0` (which the
  measuring settings below deliberately use) *guarantees* a cast, because auto white balance is the
  thing that would have normalised it. Reading that cast as a firmware defect produced a
  confident, wholly invented "double byte-swap" bug, complete with a one-line fix for code that was
  fine.
- **Shape is trustworthy; hue is not.** No camera characteristic can flip an L-shape. That is why the
  only real defect the rig has found in the display path — the panel rendering **Y-flipped** — was
  caught by shape, and why every colour-based claim so far has been withdrawn.
- **Judge relative colour, never absolute RGB.** "Which bar is red vs blue" is answerable; "is this
  exactly `(255,0,77)`" is not. Two colours can still be *told apart* by a channel: the Y-flip was
  confirmed partly by separating green `11` from yellow `10` on their **red** channel (31 vs 82) —
  a relative comparison between two markers in the same frame, which survives any cast.

### The rig resolves the LCD pixel grid — that is a feature, and a trap for scripts

At the tuned focus the individual LCD pixels are visible (that is the top of the focus curve below).
So **any automated per-pixel analysis must smooth above the grid pitch first**, or it reads the grid as
signal. This has already produced one false result: a script counting tear bands (abrupt colour steps
along a panel row) reported tears in a *demonstrably static, clean* frame, because the pixel grid
registers as steps along the same axis. Smoothing then found the panel edge instead. **Judge structure
by eye first, and only trust a detector that reproduces what the eye plainly sees on a control frame.**

### Test patterns must be asymmetric

**A symmetric pattern cannot show orientation.** The Gate #1 palette bars (16 vertical stripes) look
identical mirrored — the bar *order* reverses, which reads as "the colours are wrong" rather than
"the panel is flipped". Hours went into that confusion.

Use a pattern whose **shape** pins orientation independent of colour:

- an **L** along the top and left edges (asymmetric under both rotation and mirroring),
- a **notch** in one corner (top from bottom),
- a **stub pointing right** from centre (left from right),
- coloured corner markers **last**, to name colours only *after* orientation is settled.

`firmware/pico-e32-host` carries such a cart. Expected upright: white L on top+left, notch top-left,
red=TL, green=TR, blue=BL, yellow=BR, stub pointing right.

### The rig's own geometry is part of the measurement

- **The camera is mounted at 90°: the LEFT of the frame is the TOP of the panel.** Captures must be
  rotated 90° CW before judging anything. Forgetting this made a correctly-rendering panel look like
  an orientation bug — and the correct fix (nothing) was nearly applied to working code.
- **⚠ The camera also HORIZONTALLY-MIRRORED, and that masked a real display bug for two days.** The
  OV3660 output was left-right mirrored, which silently *cancelled* the panel's own X-flip — so a mirrored
  panel read as "fine" in every capture. A human reading the real panel caught it (2026-07-18). **A
  capture cannot verify left-right / mirror correctness — judge text direction by EYE at the panel, never
  from a capture.** Fixed in the camera fw (`set_hmirror(s,0); set_vflip(s,1)` at init, verified against
  the real panel; runtime-overridable via `/capture?hmir=0|1&vflip=0|1`). The display was actually mounted
  **180°** (not just Y) — see `boards/makerfabs-ili9488-r1/board.cpp` `ROTATE_180`.
- **The panel's own "up" is not marked on the board.** Do not assume the device's orientation from
  its shape; establish it with the asymmetric pattern.
- If the camera is moved, **everything above is invalidated** — re-check the rotation and re-sweep
  exposure.

### Measuring settings (tuned 2026-07-16)

```
/capture?awb=0&exp=600&gain=0&sat=2      # BRIGHT content (palette bars): UXGA, true-ish colour, 0% clipped
/capture?awb=0&exp=1200&gain=0&sat=2     # DARK content (a mostly-dark cart): brightest CLEAN exposure
```

- **UXGA (1600×1200)** — the panel is a small part of frame; resolution is what buys resolvable detail.
- **`awb=0`** — auto white balance renormalises hue toward grey, destroying the *relative* colour
  judgement the rig exists for. It costs an absolute cast (see the rule above).
- **Exposure is scene-dependent — pick by how bright the content is.** `exp=600, gain=0` is tuned for the
  16-colour palette bars, where anything brighter clips 24–26% of the panel. A **mostly-dark cart** (e.g. a
  `cls(1)` dark-blue screen) *under*-exposes at 600; use **`exp=1200, gain=0`** (verified 2026-07-18 on the
  fake-08 input-demo cart — clearly brighter and more legible, still colour-true).
- **`exp` caps at ~1200 on the OV3660** — `exp=1400`/`1900` clamp to the same frame. Past that, only **`gain`**
  brightens, and it **adds sensor grain and clips bright sprites toward white** — so avoid gain when judging
  colour or fine detail; `exp=1200 gain=0` is the ceiling of *clean* brightness.
- **Re-sweep after any move.** These are right for one framing, not laws of nature.

### Focus is mechanical, and the sweet spot is narrow

The OV3660 has **no autofocus** — every `af_*` hook in the driver is `NULL` (`sensors/ov3660.c`); only
the OV5640 implements it. **Distance is the only focus control**, plus the lens barrel's screw thread.

Measured on this rig with the palette bars (higher px/bar and separation = better):

| panel width in frame | px per bar | weakest bar separation | |
|---|---|---|---|
| 184 px | 10.8 | 11.1 | too far — blur ≈ one whole bar |
| 306 px | 17.8 | 14.8 | ok |
| 738 px | 43.1 | 20.9 | good |
| **968 px** | **57.0** | **19.9** | **best — LCD pixel grid visible** |
| 1235 px (clipped) | 45.8 | 9.6 | **past the near limit — bars melt together** |

**Closer helps until it doesn't.** There is a near limit, and crossing it *costs* separation while
still increasing magnification — so magnification alone is a misleading guide. Aim for the whole panel
in frame with a black margin, centred and straight, then **tape it down**.

**Use the live stream to aim:** `http://<ip>/stream?size=svga` (~18 fps). Resolution is irrelevant for
pointing; switch back to UXGA before measuring, and **close the tab** — a live stream competes with
`/capture` for framebuffers.

## Framing & camera realities

- **Keep the framing fixed** once set (tape the camera down) so captures are comparable
  across runs; a moved camera invalidates before/after comparisons.
- Account for **backlight glare, focus, and colour cast** — the panel is emissive, so the
  camera's white balance can shift hues. Judge *relative* colour (bar order, which bar is
  red vs blue) rather than absolute RGB values.
- Camera is SVGA (800×600) JPEG — ample to read a 320×480 panel. The F is a **fisheye**: expect
  barrel distortion at the edges, so centre the panel in frame.

## Evidence

Captures live in `/tmp` and are **expected to be lost** — they are throwaway diagnostics, produced
by the dozen while iterating. When a frame is genuine *evidence* for a worklog, copy it deliberately
to `docs/hardware/evidence/` and reference it; otherwise record the observation and the numbers, not
the JPEG. (`.gitignore` still lists `captures/` as a guard, in case `CAPTURE_DIR` is ever pointed at
the tree.)

## Open items

The backlog for the rig (per [`.ai/AGENTS.md`](../../.ai/AGENTS.md) → *Plan first*); reachable from
the master index [`docs/pico-e32-todo.md`](../pico-e32-todo.md).

| # | Item | Why | Verified by | Status |
|---|------|-----|-------------|--------|
| BC-1 | ~~Aim/focus the rig~~ **DONE** | The rig was never the problem — it correctly reported a backlit, unaddressed panel ([wrong pin map](../worklog/2026-07-16-panel-rev1-pinmap.md)). Now tuned + taped: **968 px panel, 57 px/bar, LCD pixel grid visible**; settings and the focus curve are documented above | 16 palette bars individually resolvable ✅ | ✅ **done** |
| BC-2 | ~~**Gate #1 visual confirm**~~ **DONE** | FPS and the draw log prove throughput, not what's on the glass | Captured frame vs the expected pattern; pass/fail stated against the frame | ✅ **done (2026-07-16)** — bars sharp at 57 px/bar; **Y-flip found, fixed and re-verified upright**; and the rig supplied the thing a counter never could: the panel is **visibly updating during the timed window** (tearing while the palette animates). See [worklog](../worklog/2026-07-16-yflip-and-gate1-fps.md) |
| BC-3 | ~~**Gate #3 visual confirm**~~ **DONE** | Framebuffer is byte-identical host↔device; only the panel transform was unproven | Captured frame vs the host-rendered reference | ✅ **done (2026-07-16)** — the L-pattern cart renders **upright** on the glass: L on top+left, notch top-left, red=TL green=TR blue=BL yellow=BR, stub right. Blob-detected and mapped back to cart coordinates, not eyeballed |
| BC-4 | **Battery operation** — confirm the board survives on battery | `POWER_HOLD_PIN` (GPIO 33) is now asserted per the vendor library, but USB masks the whole issue — the fix is unproven | Unplug USB; the board stays up and still serves `/capture` | ❓ unverified |
| BC-5 | **Port the probe diagnostics to `driver/i2c_master.h`** | They use the legacy `driver/i2c.h`, which forces `CONFIG_SCCB_HARDWARE_I2C_DRIVER_LEGACY` on the whole app; mixing the two aborts at boot | Diagnostics still run on a forced-failure map, with the override removed and no boot loop | 💤 low priority |
| BC-6 | **Label the boards physically** and record the mapping here | `.ai/AGENTS.md` → *Hardware & flashing notes* says refer to boards by a stable label; `/dev/ttyUSB*` numbering already swapped once mid-session | This doc names each board by label, and `PORT=` is looked up rather than assumed | 📋 open |

> **BC-1 is done.** The rig works and has already earned its keep: it caught the panel rendering **Y-flipped** (via the asymmetric L-pattern) — a defect fps counters and framebuffer checksums both call a pass.
>
> ⚠️ **Before touching the camera, look at the panel.** The capture data cannot distinguish
> *"camera aimed wrong"* from *"the LCD is dead and only the backlight is on"* — both give the same
> constant blue-white light. The second case means **Gate #1 does not pass** (288 fps of DMA into a blank
> screen — the exact risk its worklog named). Flash the RGB cycler and use your eyes: red/green/blue →
> aim the camera; blank or garbage → the panel init is the bug, not the rig.
