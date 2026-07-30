# 2026-07-29 — GP-3: ESP32-P4 ST7701S/MIPI-DSI display bring-up — FIRST LIGHT ✅

Goal ([`GP-3`](../hardware/pico-e32-guition-jc4880p443c-p4.md)): bring up the 480×800 ST7701S panel over
MIPI-DSI on the Guition JC4880P443C (ESP32-P4). Started as research/de-risking, then implemented and
**verified on the bench camera the same day** — the panel lights, colours are correct, orientation is
upright. Follows GP-2 (board boots — see [[2026-07-29-gp2-p4-board-scaffold]]).

> **TL;DR — DONE.** Feasibility confirmed → config sourced from ESPHome's board-specific model → hand-rolled
> the DSI driver in `board.cpp` → first light on the first flash. Serial: `board_lcd_init -> ESP_OK`,
> `ST7701S/MIPI-DSI up: 480x800, 2-lane @ 500 Mbps, DPI 34 MHz`. Camera (P4 panel, right of frame): RGB
> fills render, and the four corner markers land **RED-TL · GREEN-TR · BLUE-BL · WHITE-BR** exactly as
> drawn — so **colour order is correct (no BGR swap) and orientation is upright (no rotation needed).** The
> ESPHome-sourced config is now **hardware-validated on this exact board** — the "trust the schematic" risk
> is retired by the panel itself (the oracle). This also **satisfies GP-4** (colour + orientation on glass).

## Feasibility — MIPI-DSI on this board (IDF v6.0.2)

Checked in the vendored tree:
- **`esp_lcd/dsi`** ships the DSI bus + DBI (command) + DPI (video) driver
  (`components/esp_lcd/dsi/esp_lcd_mipi_dsi_bus.c`, `include/esp_lcd_mipi_dsi.h`).
- The **P4 DPHY** low-level layer is present (`components/esp_hal_lcd/esp32p4/include/hal/mipi_dsi_phy_ll.h`);
  `soc_caps.h` → `SOC_MIPI_DSI_SUPPORTED 1`.
- **No chip-revision guard** anywhere in the DSI path (grep for `revision`/`ESP32P4_REV`/`chip_rev` in the
  DSI/DPHY sources = empty). So the driver does not refuse early silicon at compile/runtime.
- DPHY power = an **internal LDO channel 3 @ 2500 mV** (`esp_ldo_regulator`, from the IDF DSI test board
  `components/esp_lcd/test_apps/mipi_dsi_lcd/main/test_mipi_dsi_board.{c,h}` — `TEST_MIPI_DSI_PHY_PWR_LDO_CHAN 3`,
  `..._VOLTAGE_MV 2500`). This is P4-fixed, safe to reuse.
- **No built-in ST7701 panel driver in IDF** — need the espressif managed component `esp_lcd_st7701`
  (registry) or a hand-rolled DBI init. The managed component takes a custom vendor init sequence, so it's
  the least-effort path (feed it the sequence below).

**Verdict:** DSI is viable on this board's toolchain. The one unknown only first light will settle: whether
the DPHY on **early v1.x** silicon behaves like the v3.x parts the community configs were tuned on.

## The config blocker — SOLVED from ESPHome's board-specific model

The finicky ST7701S init + timing + reset pin are the "no black screen" crux. **ESPHome merged a model for
this exact panel:** [esphome/esphome#12068](https://github.com/esphome/esphome/pull/12068),
`esphome/components/mipi_dsi/models/guition.py`, model `"JC4880P443"` — reviewed and working on hardware.

| param | value |
|---|---|
| Controller / res / color | ST7701(S) · 480×800 · RGB |
| DSI | **2 lanes @ 500 Mbps**, DPI pixel clock **34 MHz** |
| H timing | pulse **12** · back porch **42** · front porch **42** |
| V timing | pulse **2** · back porch **8** · front porch **166** |
| **LCD reset** | **GPIO5** |
| **Backlight** | **GPIO23** (LEDC PWM) — from ESPHomeDesigner #254 + search, not the DSI model |
| DPHY power | internal LDO ch **3** @ **2500 mV** |
| GT911 touch | I²C SDA **7** / SCL **8**, reset **3**, 400 kHz (addr 0x5D/0x14) |
| ES8311 audio | addr **0x18**, I²C SDA7/SCL8, I2S LRCLK **10** / BCLK **12** / MCLK **13**, mic-in **48**, spk-out **9** |

### Full ST7701S init sequence (verbatim from `guition.py`, model JC4880P443)

Each tuple is `(cmd, data...)` sent as a DSI DCS/generic write. `0xFF,0x77,0x01,0x00,0x00,0xNN` selects
ST7701 command bank NN (BK0/BK1). The framework appends the standard DCS `0x11` (sleep-out) + `0x29`
(display-on) after this vendor sequence.

```
(0xFF, 0x77, 0x01, 0x00, 0x00, 0x13)
(0xEF, 0x08)
(0xFF, 0x77, 0x01, 0x00, 0x00, 0x10)
(0xC0, 0x63, 0x00)
(0xC1, 0x0D, 0x02)
(0xC2, 0x10, 0x08)
(0xCC, 0x10)
(0xB0, 0x80, 0x09, 0x53, 0x0C, 0xD0, 0x07, 0x0C, 0x09, 0x09, 0x28, 0x06, 0xD4, 0x13, 0x69, 0x2B, 0x71)
(0xB1, 0x80, 0x94, 0x5A, 0x10, 0xD3, 0x06, 0x0A, 0x08, 0x08, 0x25, 0x03, 0xD3, 0x12, 0x66, 0x6A, 0x0D)
(0xFF, 0x77, 0x01, 0x00, 0x00, 0x11)
(0xB0, 0x5D)
(0xB1, 0x58)
(0xB2, 0x87)
(0xB3, 0x80)
(0xB5, 0x4E)
(0xB7, 0x85)
(0xB8, 0x21)
(0xB9, 0x10, 0x1F)
(0xBB, 0x03)
(0xBC, 0x00)
(0xC1, 0x78)
(0xC2, 0x78)
(0xD0, 0x88)
(0xE0, 0x00, 0x3A, 0x02)
(0xE1, 0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0, 0x00, 0x40, 0x40)
(0xE2, 0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00)
(0xE3, 0x00, 0x00, 0x33, 0x33)
(0xE4, 0x44, 0x44)
(0xE5, 0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0, 0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0)
(0xE6, 0x00, 0x00, 0x33, 0x33)
(0xE7, 0x44, 0x44)
(0xE8, 0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0, 0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0)
(0xEB, 0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00)
(0xEC, 0x08, 0x01)
(0xED, 0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B)
(0xEF, 0x08, 0x08, 0x08, 0x45, 0x3F, 0x54)
(0xFF, 0x77, 0x01, 0x00, 0x00, 0x00)
```

## Pin-map correction (the "trust the schematic" trap, again)

The board doc previously carried a cautionary source: *"reset GPIO23 + backlight GPIO22 via a Silergy
SY7023."* Against the board-specific ESPHome model that's **wrong for this board**: **LCD reset = GPIO5**,
GPIO3 is the *touch* (GT911) reset, and **GPIO23 is the backlight**. Corrected in the board doc. The one
authoritative source that would settle every pin remains the vendor package `JC4880P443C_I_W.zip`
(schematics + Arduino ST7701S demo), linked from MiniWebRadio #791 — its direct download link now serves
an HTML portal (moved), so it needs manual retrieval. Not pulled a binary package from a third-party host
over a broken link.

## Implementation (hand-rolled, no managed component)

No `esp_lcd_st7701` component is vendored, so the driver is hand-rolled in `boards/guition-jc4880p443c/board.cpp`
against IDF's built-in `esp_lcd/dsi` — self-contained, no registry download. `board_lcd_init` does:

1. `esp_ldo_acquire_channel` (ch3 @ 2500 mV) — DPHY power.
2. Reset the ST7701 on **GPIO5** (high → low 10 ms → high 120 ms).
3. `esp_lcd_new_dsi_bus` (2 lanes, 500 Mbps, `MIPI_DSI_PHY_CLK_SRC_DEFAULT`).
4. `esp_lcd_new_panel_io_dbi` (8-bit cmd/param) → send the 37-command ST7701 init via
   `esp_lcd_panel_io_tx_param`, then DCS `0x11` (sleep-out, +120 ms) + `0x29` (display-on, +20 ms).
5. `esp_lcd_new_panel_dpi` (34 MHz, RGB565 in/out, `num_fbs=1`, the timing above) → `esp_lcd_panel_init`.
6. Cache the DPI framebuffer pointer; backlight on (**GPIO23** high).

`board_lcd_blit` → `esp_lcd_panel_draw_bitmap`. `board_lcd_fill` writes the scanout FB directly + a
`esp_cache_msync` C2M flush (the FB is cached PSRAM). `board_lcd_rgb565` is standard LE RGB565, no swap.
App `REQUIRES` picked up `esp_lcd esp_driver_gpio esp_mm` (LDO is in `esp_hw_support`, always linked).

## First light — VERIFIED on the bench camera (satisfies GP-4)

Built clean (app `0x35f90`, 79 % free), flashed (`Hash of data verified`), reset + captured serial + camera:

- **Serial:** `board_lcd_init -> ESP_OK`; `board.p4: ST7701S/MIPI-DSI up: 480x800, 2-lane @ 500 Mbps, DPI 34 MHz`.
  The **entire DSI init ran with zero errors on early v1.x silicon** — LDO, DSI bus, all 37 DBI init writes,
  sleep-out/display-on, and `esp_lcd_panel_init` all returned ESP_OK. So the DPHY works on this silicon (the
  one thing feasibility couldn't settle on paper).
- **Camera (P4 panel = right of the two-panel frame, S3 Celeste idle is centre):** the RGB full-screen fills
  render (initially blown out orange — the camera was tuned dark for the S3 Celeste; **the panel was just
  bright, not miscoloured**). Dropping the relay's brightness/contrast/exposure (V4L2 via
  `http://127.0.0.1:8090/api/set`, restored after) resolved the four corner markers cleanly:
  **RED top-left · GREEN top-right · BLUE bottom-left · WHITE bottom-right** — exactly as drawn.
- **Conclusions from the glass (the oracle):**
  - **Colour order correct** — no BGR swap; `board_lcd_rgb565` standard RGB565 is right.
  - **Orientation upright, no rotation needed** — markers land in the drawn corners; the ST7701S-on-DSI
    rotation quirk (PPA hardware-rotate, esp-idf #16201) is **not** needed for this native-portrait use.
  - **The ESPHome-sourced config is validated on this exact board** — reset G5, backlight G23, 2×500 Mbps,
    34 MHz, the timing, and the init sequence all work. The schematic-confirmation risk is retired by first
    light; the vendor package is no longer on the critical path (still nice-to-have for a second source).
  - Evidence frame: `/tmp/pico-e32-captures/gp3-markers.jpg` (throwaway per bench convention).

## GP-5a — Serial input backend ported to P4 (VERIFIED over the wire)

To unblock input without physical access, ported the S3's `input_serial` backend (bytes over the console
link → PICO-8 buttons). The only board-specific part is the **transport**: the S3 reads UART0, but the
P4's console is **native USB-Serial-JTAG**. Made `components/input/input_serial.c` transport conditional on
the console config so **one file serves both boards, S3 unchanged**:

- `#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` → `usb_serial_jtag_driver_install` + `usb_serial_jtag_read_bytes`
  (the P4 board sets this console). `#else` → the existing UART0 path (S3, whose console is UART — the
  macro is unset there, so its path is byte-for-byte unchanged).
- Input component `REQUIRES` gained `esp_driver_usb_serial_jtag` (S3 links but never calls it).
- Fixed a **pre-existing latent v6 `-Werror=misleading-indentation`** in `input_serial.c`'s
  `input_sched_stats` one-liner — it never bit the S3 because its v6 builds used the `scheduled` backend, so
  `input_serial.c` hadn't been recompiled on v6 until this P4 build. (Same class as the GP-2 `input_stub.c`.)

`pico-e32-p4-hello` now pulls in **only** the `input` component (`EXTRA_COMPONENT_DIRS` → `components/input`,
`INPUT_BACKEND=serial` defaulted in the app CMakeLists) and drives a visible on-panel marker: `l/r/u/d` move
an 80×80 square, `z`=O turns it green, `x`=X red.

**Verified end to end over serial** (inject keys on `/dev/ttyACM0`, read the logs back on the same link):

```
I (6091) input.serial: serial input on USB-Serial-JTAG: l/r/u/d dir, z=O x=X p=pause (tap holds 6 frames)
I (6091) p4-hello: input backend 'serial' -> OK
 rrr -> input held=0x02  marker x 220->320   (RIGHT)
 ddd -> input held=0x08  marker y 380->480   (DOWN)
 z   -> input held=0x10                       (O)
 x   -> input held=0x20                       (X)
 lll -> input held=0x01  marker x 300->200   (LEFT)
```

Held masks match `input.h` (LEFT 0x01 / RIGHT 0x02 / DOWN 0x08 / O 0x10 / X 0x20) exactly and the marker
position tracks — so the whole host → USB-JTAG → backend → app path works on the P4.

**Camera-confirmed on the glass:** two frames (`/tmp/pico-e32-captures/gp5-marker-TL.jpg`,
`gp5-marker-BR.jpg`) show the cyan 80×80 marker rendered on the P4 panel and **moved** between shots
(top-left → lower, driven by `u`/`l` then `d`/`r`). Note rapid bytes don't accumulate linearly — several
arrive within one 33 Hz poll and collapse to one held frame — so a burst moves the marker a few hundred px,
not to the exact corner; movement + direction are unambiguous. Marker reads cyan (not red) at capture time
because the O/X colour-hold is only ~180 ms (6 frames) and lapsed before the shutter; the colour path
itself is proven by the `held=0x20` (X) / `0x10` (O) serial lines above. Exposure was dropped on the USB
relay for the bright panel, then restored to the S3-tuned values.

## GP-5b — GT911 touch (controller up; touch-point HITL deferred)

Added `board_touch_init`/`board_touch_read` to `board.cpp` (v6 `i2c_master` API) + `BOARD_HAS_TOUCH`, and
the app polls at ~33 Hz and logs points. Pins from ESPHomeDesigner #254: I²C **SDA GPIO7 / SCL GPIO8**,
400 kHz, GT911 default addr **0x5D**. GT911 uses **16-bit register addresses** (unlike the S3 FT6236's
8-bit): read status `0x814E` (bit7 ready, low nibble = count), points from `0x8150` (stride 8:
xlo,xhi,ylo,yhi,…), then write 0 to `0x814E` to release the frame.

- **Controller detection/init — HARDWARE-VERIFIED:** serial shows
  `board.p4: GT911 touch up: id "911 " @ 0x5D (SDA=7 SCL=8)` and `board_touch_init -> OK`. The product-ID
  probe (`0x8140` → "911") succeeded, so the I²C bus, the SDA7/SCL8 pins, and the 0x5D address are all
  correct and the controller responds.
- **Touch-point reporting — UNVERIFIED (honest blocker):** no touch events registered. Added a
  `TOUCH_DEBUG` build (`DEFS='-D TOUCH_DEBUG=1'`, logs the raw status register) to disambiguate a read bug
  from "no touch happened" — but the **panel is not physically reachable this session**, so no one could
  touch it. `board_touch_read` is implemented to the GT911 spec but is **not proven on hardware**, and the
  touch↔display axis alignment is likewise unconfirmed (same class as the S3's `DP-9`). Per the verify
  rule: naming the blocker rather than implying it was tested.
- **Resume:** flash `pico-e32-p4-hello` (optionally `DEFS='-D TOUCH_DEBUG=1'`), watch serial, and touch the
  panel — top-left / top-right / bottom-left / centre — to confirm points appear and axes line up with the
  display. If nothing reports, the `TOUCH_DEBUG` status log says whether the GT911 sets bit7 on touch (→ a
  parse bug) or stays 0 (→ the controller needs a reset/INT/config step to start scanning).

**Virtual-button UI ported (2026-07-30) — portable, deck camera-verified.** Brought the approved touch
control deck (`pico-e32-fake08-touch-ui.html`: d-pad + O/X + MENU) to the P4 by making `input_touch.c`
board-agnostic, same pattern as the fps HUD:
- **Geometry-derived layout.** The zones + deck positions are computed from `board_lcd_width/height` (new
  getters on the seam) as proportions of the panel + deck band. Tuned so W=320,deckH=224 reproduces the
  S3's exact hardcoded layout (dpad 92,376 / O 212,414 / X 272,352 / menu) — **S3 zones unchanged** — and
  scale to the P4 480×800 (serial: `dpad(138,547) O(318,639) X(408,489) menu(240,321) r=46`).
- **Portable deck renderer.** Removed the S3 `board.cpp`'s LovyanGFX `board_draw_touch_deck`; the deck is
  now rasterised in `input_touch.c` via `board_lcd_blit`/`board_lcd_rgb565`, computing per-pixel colour in
  the blit buffers to match the mockup: **vertically-graded d-pad bars + four direction chevrons + a dark
  hub**, **spherically-shaded O/X buttons** (radial highlight, thin salmon/teal ring), a MENU pill, and
  O/X/MENU labels from `assets/pico8_font.h`. Lives in the input component (same archive as the caller —
  the fps-HUD link lesson). Camera-confirmed the chevrons + sphere shading render on the P4.
- **Verified (camera):** built `INPUT_BACKEND=touch` (game flush-top, deck below), flashed — the deck draws
  correctly on the P4 (d-pad lower-left, O/X diagonal, MENU pill); Celeste title above it. S3 `touch` build
  links clean (no regression; build-only).
- **✅ TOUCH VERIFIED ON HARDWARE (2026-07-30):** the owner tapped the panel and **all buttons register
  correctly** — so the GT911 read *and* the zone mapping work end to end. This closes the GP-5b HITL gap
  (the read that couldn't be confirmed last session) and the whole touch path.

**Follow-up polish (2026-07-30), from owner feedback "screen looks small / d-pad looks different":**
- **Bigger screen — panel-adaptive integer scale.** `ESP32Host` now picks the upscale at runtime from panel
  width (`pico_scale`: largest fitting, clamped [2,3]): S3 320 → 2× (256², unchanged), **P4 480 → 3× (384²)**.
  `drawFrame` generalised from hard 2× to any scale; strip buffer sized per-scale (16 KB→36 KB, still internal
  DMA on the P4 — no PSRAM fallback). The deck's game-height uses the **same** scale rule so it stays flush
  below the game. fps headroom is ample (384² draw ≈ 6 ms vs the ~8 ms/frame measured at 256²).
- **D-pad matches the mockup.** Rounded bars (corner-radius mask), four direction chevrons, dark hub, and
  spherically-shaded O/X buttons — all per-pixel in the blit buffers.
- **Fidelity pass (2026-07-30), against the owner's re-sent design (byte-identical to the repo doc):** the
  first render was close in layout but off in detail. Fixed to match the mockup precisely — **MENU is now an
  outline pill** (design `fill=none`, not the filled gray I had), the **d-pad chevrons are small and sit at
  the arm tips** (were oversized/inset), the **hub is subtle** (was a prominent black dot), and **O/X are
  clean vector glyphs** — 'O' a ring, 'X' a cross — composited onto the sphere instead of the blocky 3×5
  font. Verified pixel-perfect via the framebuffer screenshot (fb.png), not the camera.
- **Background colour (owner request).** The letterbox + deck were pure black; switched to the mockup's
  subtle dark blue-grey **surface `#0f141d`** so the game screen reads as inset in a body and the deck
  controls sit on a surface. One shared tone: `ESP32Host`'s one-time panel fill + a `deck_bg()` used for all
  control buffer backgrounds (disc corners, d-pad bar corners, MENU pill interior). Host-fill change is
  shared (S3 letterbox also becomes the subtle surface instead of black — design-consistent).

**Camera-free "screenshot over serial" (2026-07-30).** Added a dev tool to see the panel pixel-perfect
without the bench camera (no distortion/exposure guesswork): `board_lcd_framebuffer()` exposes the P4's
live DPI scan-out buffer (`s_fb`, RGB565 480×800); an `FB_DUMP` build variant
(`DEFS='-D … -D FB_DUMP=1'`, P4-only) runs the loop so the game+deck composite, then streams the framebuffer
once over the console, framed by an `\xFB\xFB\xFB\xFBSHOT` magic + w/h. Host side: `tools/fb_screenshot.py`
resets the board, finds the header, reads `w*h*2` bytes, and writes a PPM (→ PNG via ImageMagick). Confirmed
the reconstructed 480×800 image shows the gradients / spherical buttons / chevrons the camera couldn't
resolve — a clean way to verify display work going forward. (S3 has no single RGB565 fb, so this is P4-only.)

## GP-7 — Feature parity: fake-08 + real Celeste on the P4 (RISC-V) ✅ core

Built the existing `pico-e32-fake08` app for the P4 board — same runtime, same cart, through the same
`board.h` seam the P4 now implements:

```
make flash APP=pico-e32-fake08 BOARD=guition-jc4880p443c PORT=…if00 BAUD=230400 \
  DEFS='-D CELESTE=1 -D FORCE_FLASH_CART=1 -D CENTER_GAME=1 -D INPUT_BACKEND=serial'
```

Three changes made it build+run, all S3-neutral by construction:

1. **Host geometry-agnostic.** `ESP32Host.cpp` hardcoded the S3's 320/480 in its `OX/OY` centring macros.
   Replaced with runtime `s_ox/s_oy` computed in `oneTimeSetup` from the panel size the app now passes to
   the (previously ignored) `Host(windowWidth,windowHeight)` ctor — `main.cpp` passes
   `Host(BOARD_LCD_H_RES, BOARD_LCD_V_RES)`. S3 stays 320×480 (`OX=32,OY=112`); P4 auto-centres 256×256 →
   `OX=112,OY=272` on 480×800 (serial confirmed: `8 strips @ 112,272 on 480x800 panel`). Upstream fake-08
   `source/` untouched — only the platform binding + the app.
2. **REQUIRES superset.** `firmware/pico-e32-fake08/main/CMakeLists.txt` gained `esp_lcd esp_mm` for the P4
   `board.cpp` (DSI + cache); the S3 links them unused. LovyanGFX compiles empty on P4 as before.
3. **★ RISC-V z8lua bug fixed (the real blocker).** First P4 boot **panicked**:
   `Lua panic: bad conversion number->int; must recompile Lua` → abort → boot-loop. Diagnosed by making
   the check print its values: `ti=-4660` (correct) but **`tu=0`** — `lua_tounsigned(fix32(-0x1234))`
   returned 0 instead of `0xFFFFEDCC`. Root cause: `llimits.h`'s generic `lua_number2unsigned` runs the
   conversion through a `floor()`/modulo path **in `double`** (fix32's implicit `operator double`), then
   casts the negative double to unsigned. That cast **wraps on Xtensa/x86 but RISC-V saturates a negative
   float to 0** (`fcvt.wu`) — so the S3 passed this check and the P4 aborted. This is exactly the "RISC-V
   re-validation" risk the plan flagged. Fix: define `lua_number2unsigned` in z8lua's fix32 config
   (`components/z8lua/luaconf.h`) to convert via **int32** (`(LUA_UNSIGNED)(int32_t)(n)`) — integer→unsigned
   is arch-independent modulo, never touches `double`. Correct on both arches; fix32's own `operator
   int32_t` (`m_bits>>16`) already yields the right value.

**Verified on the glass (camera):** real Celeste renders on the P4 panel — the **title screen**
(`gp7-celeste-title.jpg`) and, after serial input (`x`/`z` to start, `r`+`z` to move/jump), a **gameplay
room** (`gp7-celeste-play.jpg`, the snowy first level with platforms + spikes). Serial: `loading celeste.p8
(87531 bytes)` → `entering GameLoop` → `serial input on USB-Serial-JTAG`, no panic. So z8lua + the fake-08
runtime + the display + serial input all work together on RISC-V — **core feature parity with the S3.**

**Gate #2 re-validated on RISC-V (fps measured).** A `MEASURE_FPS` build (streams `step`/`draw`/max-fps
every 30 frames) driven into gameplay over serial:
```
f330: step=4.37 draw=2.70 ms | max 141.6 fps | worst 12.70 ms | heap-free 32.5M
f540: step=5.38 draw=2.71 ms | max 123.6 fps | worst 13.74 ms
f630: step=8.17 draw=2.73 ms | max  91.7 fps | worst 20.61 ms   (room-transition spike)
```
So on the P4 (RISC-V ~360 MHz): **Step (Lua _update/_draw) ~5–6 ms typical, drawFrame ~2.7 ms → ~8 ms/frame
compute → ~120 fps of headroom**; worst-frame ~13–21 ms at room transitions; a one-time 121 ms cart-load
spike at f0. That's **~2× the S3** (Xtensa 240 MHz, ~15.8 ms/frame avg) — Celeste is comfortably 30/60-fps
capable on the P4, capped to the 30 fps target by the pacer. Draw is notably cheap (2.7 ms) — the DSI blit
path is fast. Heap-free ~32 MB (PSRAM abundant). **Gameplay video captured** (`gp7-celeste-p4.mp4`, cropped
to the P4 panel, player moving/jumping around the first room).

**Shared-file caveats (re-verify on S3 before landing):** GP-7 touched two shared things —
`components/z8lua/luaconf.h` (the fix32 build, S3 uses it too) and `components/fake08/.../ESP32Host.cpp` +
the fake08 app. The z8lua fix is a strict correctness improvement (S3 currently passes the check via the
buggy-but-wrapping path; the fix makes it correct on both), and the host change is 320/480-identical on S3
— but neither was re-run on the S3 hardware this session. Also note `components/z8lua` is a **submodule**:
landing the fix needs a submodule commit + gitlink bump.

## Portable on-screen FPS HUD (both boards, one implementation)

The FPS HUD used to be per-board: the S3's `board.cpp` drew it with LovyanGFX text primitives, and the P4
had none — so a `SHOW_FPS` P4 build didn't even link (`board_lcd_draw_fps` / `g_hud_owned_by_app` undefined).
Refactored it into ONE portable renderer that draws through the board-agnostic seam:

- **New `components/fake08/fps_hud.cpp`** (parent-repo wrapper, *not* the submodule) implements
  `board_lcd_draw_fps` + `g_hud_owned_by_app`, rendering "<n> FPS" from `assets/pico8_font.h` (3×5 glyphs,
  scaled ×4) into a small RGB565 box blitted via `board_lcd_blit` at a fixed top-left margin. No per-board
  text code, no `board.h` geometry — only the `board_lcd_*` seam (declared in `fake08_board.h`), so it works
  on any board.
- Removed the S3 `board.cpp`'s bespoke `board_lcd_draw_fps` + `g_hud_owned_by_app` and the stale
  `board_lcd_draw_fps` decl from the S3 `board.h`.
- **Link detail (why it lives in the fake08 component):** `ESP32Host.cpp` (fake08 archive) calls the HUD;
  putting `fps_hud.cpp` in the same component makes the symbols resolve intra-archive on every board. (In the
  app's `main` archive they went unpulled — `board_lcd_blit` only resolves because `board.o` is force-pulled
  via `board_lcd_init`; the HUD had no such anchor.)
- **Verified:** P4 `SHOW_FPS` build now links + runs — the panel shows a green **"60 FPS"** HUD over Celeste
  (camera). S3 `SHOW_FPS` build links clean (no regression; build-only this session).
- **Note on 60 vs 30:** this is `ESP32Host`'s *generic* meter (counts the 60 Hz render-loop resumes). For the
  true game-frame rate (30 for Celeste), build with `-D TELEMETRY=1` — then `main.cpp` owns the HUD
  (`g_hud_owned_by_app`) and draws the achieved game-frame fps, exactly like the S3 demo the user sees.

## Open / next

- **Board state:** holds the **P4 Celeste `SHOW_FPS` build** (`pico-e32-fake08`, CELESTE + serial input +
  on-panel FPS HUD) — a known-good running/demo state. Camera restored to its S3-tuned settings.
- **GP-7:** ✅ complete (parity + fps measured). Video: `gp7-celeste-p4.mp4`.
- **GP-5 tail:** HITL touch-point verification when the panel is reachable.
- **GP-6:** ES8311 audio (backlogged).
- **Landing:** everything is on branch `gp2-p4-board-scaffold` (uncommitted; work-hours rule). The branch has
  grown well beyond "scaffold" (GP-2→GP-7) — rename it or split into follow-on PRs; the z8lua submodule bump
  and the shared-component changes want their own review + an S3 re-verify.

## Sources

- [esphome/esphome#12068](https://github.com/esphome/esphome/pull/12068) — the JC4880P443 model (init + timing + reset pin); file `esphome/components/mipi_dsi/models/guition.py`.
- [koosoli/ESPHomeDesigner#254](https://github.com/koosoli/ESPHomeDesigner/discussions/254) — a working hardware profile (backlight GPIO23, GT911 SDA7/SCL8/rst3, ES8311 pins).
- [schreibfaul1/ESP32-MiniWebRadio#791](https://github.com/schreibfaul1/ESP32-MiniWebRadio/issues/791) — this exact board; points to the vendor `JC4880P443C_I_W.zip` package.
- IDF v6.0.2 vendored: `components/esp_lcd/dsi/*`, `components/esp_lcd/test_apps/mipi_dsi_lcd/main/test_mipi_dsi_board.{c,h}` (LDO ch3 @ 2500 mV), `components/esp_hal_lcd/esp32p4/include/hal/mipi_dsi_phy_ll.h`.
