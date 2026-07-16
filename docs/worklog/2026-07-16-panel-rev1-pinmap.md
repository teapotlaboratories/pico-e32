# 2026-07-16 — The panel was never broken: this board is rev 1, and its LCD is on different pins

**Status:** ✅ **RESOLVED. The display works.** After ~two days of concluding the panel was dead, the
cause was the **pin map**: this unit is Makerfabs' **first revision**, whose LCD sits on
**WR=35 / DC=36 / CS=37** — not the **18 / 17 / 46** their *current* board (and their current example
code) uses. Every command we ever sent went to pins this board does not wire to the panel.

**The one-line lesson:** *"verified against the vendor's own source"* is worthless if you never check
**which revision that source describes**.

---

## The fix

```c
/* firmware/*/main/*: rev-1 map — LCD on 35/36/37, free because N16R2 is QUAD PSRAM */
.pin_wr = 35, .pin_dc = 36, .pin_cs = 37, .pin_bl = 45, .pin_rd = 48,
.pclk_hz = 40 * 1000 * 1000,
```

Data bus `D0–D15 = 47,21,14,13,12,11,10,9,3,8,16,15,7,6,5,4` is **identical on both revisions** —
only WR/DC/CS moved. That is why nothing looked obviously wrong.

## Why two maps exist — it is the PSRAM

| board | PSRAM | GPIO 35/36/37 | LCD WR / DC / CS |
|---|---|---|---|
| **rev 1 (this unit)** | **N16R2 — 2 MB quad** | **free** | **35 / 36 / 37** |
| current rev | N16R8 — 8 MB octal | **consumed by PSRAM** | 18 / 17 / 46 |

ESP32-S3 **octal** PSRAM occupies GPIO 35/36/37. Rev 1's **quad** part leaves them free, so the LCD
used them. When Makerfabs moved to the octal N16R8, they had to relocate the LCD — and their current
repo documents only the new map, with nothing marking which board it belongs to.

The clue was in our own docs the whole time: `pico-e32-makerfabs-boards.md` said **N16R2, 2 MB quad
PSRAM (owner-confirmed; Makerfabs' current rev ships N16R8/8 MB, but this unit is the older one)** —
and the owner said *"this is the first revision"* twice. It was read as a footnote about memory size,
never as a fact about **pin assignment**.

## What the failure looked like (why it read as dead hardware)

Backlit white panel, forever. Meanwhile **everything that could report success, did**:

- `esp_lcd` i80 bus created, DMA completed, **288 fps** sustained,
- framebuffer checksums **byte-identical host↔device**,
- panel init returned `ESP_OK`; the `esp_lcd_ili9488` component logged *"Initialization complete"*,
- LovyanGFX's own `init()` returned true.

All true. All meaningless — the bus was clocking into pins with nothing on the other end. This is
exactly the failure Gate #1's own worklog predicted in writing: *"a broken panel init could still clock
DMA at 288 fps into a blank screen."* It was right, and it still took two days.

## Everything that got eliminated first (all correct, all irrelevant)

| suspect | test | verdict |
|---|---|---|
| camera aimed wrong | toggled backlight (GPIO 45) — capture swung **92** | ❌ camera images the panel |
| camera can't see LCD content | black↔white at fixed exposure — swing **0.3**; R→G→B — hue frozen | ✅ true, and correctly measured |
| RD floating | drove GPIO 48 high (vendor does too) | ❌ no change |
| pclk too fast | 20 → 10 MHz (vendor's value) | ❌ no change |
| hand-rolled init | swapped to `esp_lcd_ili9488` component | ❌ no change |
| the SDK | built Makerfabs' **own** IDF example with our IDF | ❌ **also white** |
| the panel is dead | — | ❌ **owner: "it works from the factory"** |

**The two facts that broke it open were the owner's**, not measurements: *"the LCD works from the
factory, only after you flash it does it stop"* (kills dead-hardware) and *"this is the first revision"*
(the actual answer). The decisive move was the owner's too: *"check this repo and the IDF version it
uses"* → [`radiosound-com/makerfabs-parallel-tft-lvgl-lgfx`](https://github.com/radiosound-com/makerfabs-parallel-tft-lvgl-lgfx),
whose `LGFX_MakerFabs_Parallel_S3.hpp` shows **WR 35, RS 36, CS 37** — written for the older board and
credited to Makerfabs' own factory sketch `firmware/SD16_3.5/SD16_3.5.ino`.

**Makerfabs' own LVGL example failing was the most misleading evidence of all.** It built and booted
cleanly on this board and still showed nothing — which read as *"even the vendor's code can't drive it,
so the hardware is dead."* In fact it is written for their **newer** board. A vendor example is only
authoritative for **the revision it targets**.

## Driver: now LovyanGFX

`components/ili9488` delegates to **LovyanGFX** (submodule, pinned `1.2.25`, FreeBSD licence), with the
rev-1 config from the factory sketch: `Bus_Parallel16` + `Panel_ILI9488`, `dlen_16bit=true`,
`pin_cs=37`, `pin_rst=-1`, 40 MHz. Public API unchanged, so both apps compile untouched.

This is the **other branch of the plan**, which was there all along:
`docs/pico-e32-development-plan.md` §A1 — *"start from `atanisoft/esp_lcd_ili9488` **or a LovyanGFX
`Bus_Parallel16` config**"*. The original Gate #1 driver followed **neither** (hand-rolled); the first
branch was then forced repeatedly after it failed. **`esp_lcd` i80 is untested on the correct pins** —
it may well work now; it simply was never tried after the map was understood.

## What the camera proved, in the end

The rig was **right the whole time** and was disbelieved. It reported a uniform blue-white field that
never responded to LCD content — which is precisely what a backlit, unaddressed panel looks like. Two
theories were built to explain it away (*camera sees backlight spill*, *panel is dead*) rather than
doubting the pin map.

Once the pins were right, the same rig immediately produced real findings:

- ✅ 16 vertical palette bars, 256×256 centred — **57 px/bar**, LCD pixel grid visible
- ⚠️ **Panel renders Y-flipped** — caught by an **asymmetric L-shape** pattern, so it is
  camera-artifact-proof. Still open (MADCTL / LovyanGFX `mirror`).
- ⏸️ Colour: **not assessable** with this rig — see the bench doc's *"What a capture can and cannot
  tell you"*. No gross error (blue is blue, red is red); a confidently-diagnosed "double byte-swap" was
  invented from camera colour casts and **withdrawn**.

## Now unknown again

**Gate #1's 288 fps is void** — it was DMA into unconnected pins, at 20 MHz on `esp_lcd`. The real
number is unmeasured: the bus is now LovyanGFX at 40 MHz, actually driving a panel. That measurement
has never been taken.

## Next

- **Fix the Y-flip**, verify against the L-pattern.
- **Measure Gate #1 honestly** — first fps number that means anything.
- Consider retrying `esp_lcd` i80 on the correct pins (a 132 MB LovyanGFX submodule is a real cost).
- Extend the Gate #2 bench to levels 16–30 (see [host-graphics worklog](2026-07-15-host-graphics.md)).
