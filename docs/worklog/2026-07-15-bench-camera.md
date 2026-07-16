# 2026-07-15 — Bench camera: the hardware-in-the-loop capture rig

**Goal:** stand up the bench camera that Gate #1 and Gate #3 have been blocked on. Both gates
are throughput- and checksum-verified but have never been confirmed against *what is actually on
the glass* — the thing `.ai/AGENTS.md` → *Verifying changes* calls the primary mechanism for
display work.

**Status:** ✅ **rig works** (M5Stack Timer Camera F: OV3660 detected, JPEG over HTTP,
`tools/capture_frame.sh` end-to-end).

> ## ⚠️ READ THIS FIRST — the conclusions below were WRONG
>
> This log spends its length building toward *"the panel is dead / Gate #1 fails"* and
> *"the camera is too close to focus"*. **Both are false.** The panel was fine; the camera was fine.
> **We were driving the wrong pins** — this board is Makerfabs' **first revision**, whose LCD is on
> **WR=35, DC=36, CS=37**, not the 18/17/46 of their newer board. Resolved 2026-07-16:
> [`2026-07-16-panel-rev1-pinmap.md`](2026-07-16-panel-rev1-pinmap.md).
>
> The reasoning is left intact rather than rewritten, because *how* it went wrong is the useful part:
> every measurement in here was correct, and every conclusion drawn from them was wrong, because the
> one assumption never tested was the pin map — it had been "verified against the vendor's own
> source" without checking **which board revision that source described**.

> **Process note, up front:** this worklog was written at the *end* of the session, not updated as
> the work happened, which is exactly what the worklog rule tells you not to do. The findings below
> were reconstructed while still fresh and every number is re-read from the logs of the runs that
> produced them, but the rule exists because that reconstruction is lossy. Recording the miss rather
> than quietly backfilling a tidy narrative.

---

## Hardware on the bench

| role | board | chip | notes |
|------|-------|------|-------|
| device under test | Makerfabs ILI9488 | ESP32-S3 N16R2 | the panel |
| bench camera (**dead**) | Espressif ESP-EYE v2.1 | ESP32-D0WD + OV2640 | sensor never answered |
| bench camera (**works**) | M5Stack Timer Camera F | ESP32-D0WDQ6-V3 + OV3660 | fisheye, 8 MB PSRAM |

Ports are **not stable** across replugs — over this session the S3 was on `ttyUSB0`, then the
camera took `ttyUSB0` and the S3 moved to `ttyUSB1`. Identify by chip, never by number:

```sh
esptool.py -p /dev/ttyUSB0 flash_id | grep 'Chip is'
# ESP32-D0WDQ6-V3 -> bench camera      ESP32-S3 -> board under test
```

## Attempt 1 — the ESP-EYE, and how it turned out to be dead

Every boot ended the same way:

```
E (1474) camera: Detected camera not supported.
E (1474) camera: Camera probe failed with error 0x106(ESP_ERR_NOT_SUPPORTED)
```

`0x106` is ambiguous, which cost time: reading `esp_camera.c`'s `camera_probe()` shows it covers
**both** "nothing ACKed on the bus" *and* "something ACKed but its PID is unknown" — the probe loop
swallows per-address failures (`if (ESP_OK != SCCB_Probe(addr)) continue;`). The distinguishing log
line (`Camera PID=0x..`) never printed, so the error alone could not separate the two.

Hypotheses tried, and what killed each:

| # | Hypothesis | Test | Outcome |
|---|-----------|------|---------|
| 1 | GPIO 13/14 need a pull-up before init (a real documented ESP-EYE quirk) | mirrored arduino-esp32's `CameraWebServer.ino` | **no change** |
| 2 | Wrong pin map | checked against arduino-esp32 `camera_pins.h` **and** esp-who `Camera_connections.md` | **16/16 match** — not it |
| 3 | `sccb-ng` (new I2C master driver) is unexercised on the classic ESP32 | forced `CONFIG_SCCB_HARDWARE_I2C_DRIVER_LEGACY` | **identical failure** |
| 4 | Sensor won't answer at 20 MHz XCLK | full 0x03–0x77 bus scan at 20 **and** 10 MHz | **0 devices** either way |
| 5 | Bus is dead / unpowered on the ESP32 side | drove internal pull-**down** on SDA+SCL, read them | both read **HIGH** → external pull-ups present, bus alive |
| 6 | Some other pin pair is the real SCCB bus | swept **all 306** output-capable pin pairs × {0x30, 0x3C}, XCLK running | **0 hits** |
| 7 | A power-down/reset line the map calls `-1` holds the sensor off | handed 5 known board pin maps to the vendor driver (it does the PWDN/RESET sequencing) | **all** returned `0x106` |

**Conclusion: the OV2640 is dead.** It answers on no pin pair, at no address, under no XCLK, with
either I2C driver — while the ESP32 side is provably healthy (bus pull-ups present, XCLK configured
`ESP_OK`). The owner confirmed the connector was firmly seated, which killed the last physical
theory. Confirmed retroactively when **the same firmware brought the Timer Camera up first try**.

**A bug I introduced while diagnosing:** the TTGO T-Journal map probes **GPIO 17, which is a PSRAM
pin on WROVER modules** — including the ESP-EYE's. Probing it took PSRAM out and panicked the board
into a boot loop (that's why early logs repeat). The map is deliberately omitted now, with the
reason recorded in `cam_main.c`.

**Caveat on my own evidence:** the pull-ups in test 5 are almost certainly on the ESP-EYE mainboard,
not the camera module — so that test proves the ESP32 side is fine, **not** that the module was
powered. It does not rescue the module, but it is weaker evidence than it first looks.

## Attempt 2 — M5Stack Timer Camera F: worked, after two traps

```
I (1460) sccb: pin_sda 25 pin_scl 23
I (1510) camera: Camera PID=0x3660 VER=0x00 MIDL=0x00 MIDH=0x00
I (1510) camera: Detected OV3660 camera
I (1510) camera: Detected camera at address=0x3c
I (1870) bench-cam: camera up (SVGA JPEG)
I (3320) bench-cam: ==== bench-cam ready:  http://192.168.7.135/capture  ====
```

### Trap 1 — the flash baud (cost the most time)

`esptool.py flash_id` connected every time; `make flash` **never** did:

```
Chip is ESP32-D0WDQ6-V3 (revision v3.0)     <- syncs fine at 115200
Changing baud rate to 460800
A fatal error occurred: Unable to verify flash chip connection (No serial data received.)
```

`idf.py` flashes at **460800** by default; plain `esptool.py` uses 115200. This board's FTDI bridge
(`0403:6001`) cannot sustain the switch. The symptom is misleading — it reads as a dead board or a
bad cable, not a baud problem, precisely because the initial sync *succeeds*.

**Fix:** a per-board `boards/<BOARD>/board.mk` hook in the Makefile, with `BAUD ?= 460800` and
`boards/m5stack-timer-cam/board.mk` pinning `BAUD := 115200`. Verified all three paths:

| BOARD | BAUD | |
|---|---|---|
| `makerfabs-ili9488` | 460800 | default |
| `m5stack-timer-cam` | 115200 | pinned by `board.mk` |
| `m5stack-timer-cam BAUD=921600` | 921600 | command line still overrides |

### Trap 2 — port permissions

The serial node is `root:dialout` and is **re-created on every board reset**, so a one-off `chmod`
evaporates. A udev rule was tried first and worked, but the owner ruled it out; the mechanism is now
plain group membership (`usermod -aG dialout`), which persists and needs no system-wide rule.
`sg dialout -c '...'` covers a shell that predates the change. No udev anywhere.

## The SCCB driver override: right setting, wrong reason

While cleaning up I removed `CONFIG_SCCB_HARDWARE_I2C_DRIVER_LEGACY`, because I had added it during
the ESP-EYE hunt on a theory that was **wrong** (I'd written that the new driver "does not ACK" —
in fact both drivers failed, because the hardware was dead). Removing it **boot-loops the working
camera 16 times in 22 s**:

```
E (1408) i2c: CONFLICT! driver_ng is not allowed to be used with this old driver
abort() was called at PC 0x400f1640
```

**The real reason:** `cam_main.c`'s probe diagnostics use the legacy `driver/i2c.h` API, and ESP-IDF
refuses to run both I2C drivers in one app — so SCCB must be pinned to *match* the diagnostics. The
setting was accidentally correct while its comment was actively misleading, which is what led me to
delete it. Restored, with the true constraint and the condition for removing it (port the
diagnostics to `driver/i2c_master.h`) recorded in `firmware/pico-e32-bench-cam/sdkconfig.defaults`.

**Lesson worth keeping:** a config comment that states the wrong reason is worse than no comment. It
survives exactly long enough to make the next person delete the line.

## Verified against the vendor's own library

The pin map was originally written **from memory** and cited to `arduino-esp32/camera_pins.h` — a
file I had not opened. That violates *Research & citations* ("verify every cited `file:line` — do
NOT cite from memory"). Checked properly against the source of record,
[`m5stack/TimerCam-arduino`](https://github.com/m5stack/TimerCam-arduino):

- **`src/utility/Camera_Class.h:6-22`** — all **16 signals match** (PWDN −1, RESET 15, XCLK 27,
  SIOD 25, SIOC 23, D7–D0 = 19/36/18/39/5/34/35/32, VSYNC 22, HREF 26, PCLK 21). `xclk_freq_hz`,
  LEDC timer/channel, JPEG, `fb_count=2`, PSRAM and `GRAB_LATEST` match too. Deliberate divergence:
  SVGA/quality 10 rather than the vendor's UXGA/quality 16 — 800×600 is ample to read a 320×480
  panel and keeps frames small over HTTP.
- The 5 diagnostic maps in `identify_board()` were also from memory; each re-verified **16/16**
  against the real `camera_pins.h` (ESP-EYE, AI-Thinker, WROVER-KIT, M5Stack PSRAM, M5Stack WIDE).

Memory turned out accurate in all six cases — which is luck, not method, and would not survive the
next board.

**The gap this found — `POWER_HOLD_PIN`:**

```c
// Power_Class.h:13
#define POWER_HOLD_PIN 33
// Power_Class.cpp:7-8
pinMode(POWER_HOLD_PIN, OUTPUT);
digitalWrite(POWER_HOLD_PIN, HIGH);
```

The Timer Camera **latches its own power rail** — it stays up only while GPIO 33 is driven high. The
code (inherited from the ESP-EYE version, which has no such pin) never did this. USB masks it
completely, which is why the rig worked; on battery the board would switch itself off, since driving
that same pin **low** is precisely how the vendor implements `powerOff()`. Now asserted first thing
in `app_main()`. GPIO 33 collides with no camera signal (D0 = 32, D2 = 34).

## Screen capture: the panel is right, the camera can't see it

The S3 is drawing correctly — confirmed over UART, camera-independently:

```
I (837)  trackA: palette bars drawn at (32,112) 256x256
I (5847) trackA: ==== Gate #1: 1441 frames in 5.00s = 288.0 fps (256x256 blit) ====
```

But every frame is a uniform glow. **Refuted hypothesis:** overexposure — auto-exposure metering the
dark box and clipping the emissive panel to white. Manual exposure/gain control was added
(`/capture?exp=&gain=`) and swept:

| exp (gain 0) | 4 | 12 | 30 | 60 | 120 | 300 |
|---|---|---|---|---|---|---|
| bytes | 11054 | 11515 | 12109 | 11878 | 11885 | 12619 |

`exp=4` is evenly black, `exp=300` an even white glow. **Clipping would reveal structure as you
darken it; only brightness changed** — so the lens is resolving nothing. The camera is too close to
focus, averaging the whole screen into one patch (the faint cyan cast is plausibly the mean of the
palette bars). No exposure value fixes distance — this is a bench fix (~10–15 cm, and the Timer
Camera F's lens is screw-adjustable). The exposure control is kept: a lit panel in a dark box does
genuinely defeat auto-exposure, so it will be needed once focused.

## Follow-up (same day): it is not a focus problem — the camera never sees the LCD

Re-tested while capturing Gate #3's cart, and the earlier "too close to focus" read was **wrong**.

First hint: the capture's mean colour ratio is **constant** (~28:35:37) across the palette-bars cart,
the trivial cart, and every exposure (auto / 300 / 200 / 60), with **0% clipped pixels** — while the
framebuffer's true ratio is **9.7:37.8:52.5**. A defocused image is a low-pass filter: it destroys
detail but *preserves* mean colour. A blur would still track content. This doesn't.

Decisive test — panel alternating **full-black ↔ full-white** every ~4 s, camera exposure **fixed**
at `exp=200`, sampled 12× over 14 s:

| | mean brightness |
|---|---|
| min | 95.7 |
| max | 96.0 |
| **swing** | **0.3** |

Black→white on a real LCD is a swing of **well over 100**.

**Control (this is what makes it conclusive):** the device's `fb_hash` alternated between exactly two
values — `38699dc5` and `f62fddc5` — which match independently-computed FNV-1a hashes of an all-`0` and
all-`7` framebuffer. So the panel was **provably** being blitted full black then full white while the
camera saw a flat 96.

**Conclusion:** the camera is seeing **light that never passes through the LCD** — backlight spill around
the panel edge, or it is aimed at the bezel/side/back rather than the screen face. Brightness responds to
*exposure* (150→79→47) but not to *content*, which is exactly what a constant backlight looks like.

**BC-1 rewritten:** the fix is **aim**, not distance. Moving it back 10–15 cm would not have helped, and
"it's out of focus" would have sent the next session chasing the lens. The cheap acceptance check is now
the alternator itself: get the swing above 100 *first*, then worry about sharpness.

### RGB test — and the ambiguity it exposes

Stronger than black/white: the backlight is white and constant, so cycling the panel through pure
**red → green → blue** (`cls(8)`/`cls(11)`/`cls(12)`, ~3.7 s each) must swing the captured *hue* if the
LCD is being imaged. Control confirmed first — device `fb_hash` cycled exactly the three predicted
values (`82199dc5` red, `00c3ddc5` green, `74d89dc5` blue, matching independently-computed FNV of an
all-8 / all-11 / all-12 framebuffer).

Camera, exposure fixed, 14 samples over ~12 s (3+ phases):

| | R | G | B | dominant |
|---|---|---|---|---|
| every sample | 27.5 | 34.7 | 37.8 | **B**, always |

Frozen. Truth is red `(255,0,77)`, green `(0,228,54)`, blue `(41,173,255)` — the hue must have swung.
It never moved.

**The ambiguity — two hypotheses fit this data identically:**

| | explanation | implication |
|---|---|---|
| **A** | The camera is aimed at backlight spill / the bezel / the panel's side, not the screen face | a bench-aim fix; the panel is fine |
| **B** | **The panel's LCD is not displaying at all** — backlight on, image dead | **Gate #1 is failing**, and 288 fps is DMA into a blank screen |

**No camera measurement can separate them** — both produce exactly this constant blue-white light. And
B is precisely the failure the Gate #1 worklog warned about: *"a broken panel init could still clock DMA
at 288 fps into a blank screen."* The `fb_hash` host↔device match does not rescue it either: that proves
the *framebuffer* is right, not that the glass renders it.

**A human eye settles it in two seconds**, which is currently the only instrument that can. The RGB
cycler is **left flashed** for exactly that: look at the panel — red/green/blue means A (aim the camera);
blank/white/garbage means B (the ILI9488 init is broken and Gate #1 does not pass).

## The rig's first real catch: **the panel is not displaying — Gate #1 does not pass**

The owner confirmed the camera is aimed at the correct screen, which killed hypothesis A. Then a test
settled it without needing an eyeball: **toggle the backlight (GPIO 45) every 3 s** with camera exposure
fixed.

| backlight | camera mean |
|---|---|
| ON | ~95.5 |
| OFF | **3.5** |
| **swing** | **92.3** |

The camera **is** imaging this panel — light from it reaches the sensor and tracks the backlight
perfectly. Yet the same camera shows **zero** response to LCD content (brightness swing 0.3 across
black↔white; hue frozen at 27.5:34.7:37.8 across red→green→blue), while `fb_hash` proves the device is
cycling the right framebuffers.

**Conclusion (over-claimed at first — corrected below): the backlight is on and nothing in the captured
light responds to LCD content.**

> ⚠️ **Correction.** I first wrote this up as "the LCD is not modulating — Gate #1 fails", which the
> evidence does not support. The backlight test proves the camera sees **this panel's backlight**; it does
> **not** prove the camera sees light that has passed **through the LCD**. Backlight spill at the bezel or
> panel edge fits *every* measurement here identically — it would follow the backlight and ignore content.
> So two hypotheses remain live, and no camera measurement can separate them:
>
> | | explanation | implication |
> |---|---|---|
> | **A′** | camera sees backlight **spill**, not the active area | rig aim; panel may be fine |
> | **B** | the LCD really is not rendering | Gate #1 does not pass |
>
> **Only a human eye can settle it.** The RGB cycler is left flashed for exactly that.
**Gate #1 does not pass** — its 288 fps is DMA into a blank screen, the exact risk its own worklog named:
*"a broken panel init could still clock DMA at 288 fps into a blank screen."* The host↔device `fb_hash`
match does not save it either: that proves the framebuffer is right, not the glass.

**This is what the bench camera exists for.** Throughput, checksums and a clean build all said "pass".
Only pointing a camera at the thing found it.

### Suspects eliminated (each with a live control)

| suspect | test | verdict |
|---|---|---|
| camera aimed wrong | backlight toggle → swing 92.3 | ❌ camera images this panel |
| RD floating | drove GPIO 48 high — vendor does this too (`board.c:131`) | ❌ no change |
| pclk too fast | 20 → **10 MHz**, the vendor's actual value | ❌ no change |
| pin map wrong | all 16 data pins + WR/DC/CS/BL/RD verified against the vendor's own `config.h` | ❌ exact match |

Pin map verified against
[`Makerfabs-ESP32-S3-Parallel-TFT-with-Touch`, `IDF/matouch/main/boards/3-5-ili9488-ft6236/config.h`](https://github.com/Makerfabs/Makerfabs-ESP32-S3-Parallel-TFT-with-Touch/blob/main/IDF/matouch/main/boards/3-5-ili9488-ft6236/config.h)
— data `47,21,14,13,12,11,10,9,3,8,16,15,7,6,5,4`, WR 18, DC 17, CS 46, BL 45, RD 48, **RST = NC**. Our
`ili9488_config_t` matches it exactly. The board reference doc was right.

### The remaining suspect — we diverged from the plan

The vendor **does not hand-roll an init**. `board.c` uses the **`esp_lcd_ili9488` component**
(`esp_lcd_new_panel_ili9488` → `esp_lcd_panel_reset` → `esp_lcd_panel_init`), with `bits_per_pixel = 16`
and `rgb_ele_order = BGR`. Our `components/ili9488/ili9488.c` hand-rolls a ~15-command sequence instead.

That is a **divergence from the plan of record**, which for A1 says: *"start from
[`atanisoft/esp_lcd_ili9488`](https://github.com/atanisoft/esp_lcd_ili9488) or a LovyanGFX
`Bus_Parallel16` config"* (`docs/pico-e32-development-plan.md:130`). The hand-rolled init is now the
prime suspect, and the plan already named the fix.

Also noted for later: the vendor sets `DISPLAY_INVERT_COLOR = true`; our init never sends `0x21` (INVON).
That would flip colours, not blank the screen — so it is not the cause, but it is likely needed once
pixels appear.

**Done — and it did not fix it.** `components/ili9488` now delegates bring-up to
`atanisoft/esp_lcd_ili9488` (1.1.1), keeping our public API so both apps compile unchanged. It inits
cleanly (`Configuring for BGR color order` → `Initialization complete`, no errors), with RD high, 10 MHz
and the vendor's exact `esp_lcd_panel_dev_config_t`. The captured hue is **still frozen** at 27.6:35.0:37.4.

So the init sequence was **not** the bug either. Worth keeping regardless: it is what the plan said to do,
it is vendor-proven, and RD-high is genuinely required. But the fault lies elsewhere — and per the
correction above, "elsewhere" may be the camera seeing spill rather than the panel being dead.

## What's verified / what isn't

**Verified:** camera detected (`PID=0x3660`) and up; WiFi + `GET /capture` serving real 800×600
JPEGs; `capture_frame.sh` end-to-end (~14 KB frames); single clean boot, no loop; per-board BAUD
resolution (all 3 cases); pin map 16/16 vs vendor source; power-hold asserted with no pin conflict.

**Not verified:** **Gate #1** (palette bars) and **Gate #3** (cart image) remain visually
unconfirmed — the rig is not imaging the panel at all (see the follow-up above). Per *Verifying changes*, naming the
blocker rather than implying these are done. Also unverified: **battery operation** — the power-hold
fix is correct against the vendor source and harmless on USB, but nothing here has been run off USB
power to prove it.

## Commands actually run

```sh
make flash APP=pico-e32-bench-cam BOARD=m5stack-timer-cam PORT=/dev/ttyUSB0 \
     WIFI_SSID='…' WIFI_PASS='…'          # creds are build-time macros, never in the tree
make flash APP=pico-e32-display-test BOARD=makerfabs-ili9488 PORT=/dev/ttyUSB1
tools/capture_frame.sh gate1-bars          # -> captures/<ts>-gate1-bars.jpg
```

Monitoring used a raw pyserial reader rather than `make monitor`: `idf.py monitor` requires a TTY and
exits 1 non-interactively in this environment. Worth noting as a real deviation from the documented
flow.

## Currently flashed

- **S3** (`ttyUSB1`): `pico-e32-display-test` — Gate #1 palette bars, 288 fps.
- **Camera** (`ttyUSB0`): `pico-e32-bench-cam` — serving `http://192.168.7.135/capture`.

## Next

- **Bench fix:** back the camera off to ~10–15 cm, aim at the panel face, adjust the lens; then
  close the Gate #1 and Gate #3 visual confirms. Framing should then be taped down so captures stay
  comparable across runs.
- **Camera-independent alternative:** the host build (`-DHOST_MAIN`) can render to PNG, which would
  verify rendering *logic* without the rig — it would not cover the panel's byte-order/MADCTL, which
  only a capture can.
- Port the probe diagnostics to `driver/i2c_master.h` so the SCCB override can go.
