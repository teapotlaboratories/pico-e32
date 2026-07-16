# Makerfabs board reference — verified pinouts

Verified from the Makerfabs product pages/wikis and the vendors' own LovyanGFX / config
sources. Full context and the design rationale are in
[`../pico-e32-development-plan.md`](../pico-e32-development-plan.md) (§2, §2b).

---

## Board 1 — ESP32-S3 Parallel TFT with Touch (ILI9488), 3.5" — **the first device**

- **Module:** ESP32-S3-WROOM-1-**N16R2** — 16 MB flash, **2 MB quad PSRAM** (owner-confirmed;
  Makerfabs' current rev ships N16R8/8 MB, but this unit is the older one).
- **Display:** ILI9488, 320×480, **16-bit Intel-8080 (i80) parallel**, RGB565, ~20 MHz pclk,
  panel has its own GRAM. Backlight PWM on **GPIO45**.
- **Touch:** FT6236 capacitive, I²C **0x38**, SDA=38 / SCL=39 (polled; INT/RST NC).
- **microSD:** SPI 1-bit — CS=1, MOSI=2, MISO=41, CLK=42 (bus shared with the LCD).
- **Expansion:** Mabee (Grove) = I²C on 38/39 + 1 GPIO (**GPIO40**, not ADC). ~1 free GPIO
  overall → buttons via an I²C expander at addr ≠ 0x38.
- **USB:** dual USB-C — native (19/20) + CP2104 UART (43/44). **Power: USB-C 5 V only, no
  battery/charger.**
- **Buttons:** only BOOT (GPIO0) + Reset.

| Signal | GPIO |
|--------|------|
| LCD D0–D15 | 47, 21, 14, 13, 12, 11, 10, 9, 3, 8, 16, 15, 7, 6, 5, 4 |
| **WR / RD / DC(RS) / CS** | **35 / 48 / 36 / 37** ← **rev 1**; see the revision warning below |
| Backlight | 45 (PWM) |
| Touch I²C SDA / SCL | 38 / 39 |
| microSD CS / MOSI / MISO / CLK | 1 / 2 / 41 / 42 |

Board config: [`boards/makerfabs-ili9488-r1/`](../../boards/makerfabs-ili9488-r1/) — the **`-r1`
suffix is deliberate**: the revision is part of the board's identity here, because rev 1 and the
current rev need *different LCD pins*. A future N16R8 unit would be a separate `-r2` board dir with
its own `board_pins.h`, not an edit to this one.

> ## ⚠️ THIS BOARD HAS TWO PIN MAPS. The LCD pins depend on the PSRAM part.
>
> **This unit is Makerfabs' FIRST REVISION (N16R2, 2 MB *quad* PSRAM). Its LCD is on WR=35,
> DC=36, CS=37.** Using the newer board's map (18/17/46) leaves the panel backlit-white while
> DMA completes normally and framebuffer checksums stay correct — it looks exactly like dead
> hardware. That mistake cost roughly two days here; see
> [worklog](../worklog/2026-07-16-panel-rev1-pinmap.md).
>
> **Why the maps differ — it is the PSRAM:**
>
> | board | PSRAM | GPIO 35/36/37 | LCD WR / DC / CS |
> |---|---|---|---|
> | **rev 1 (this unit)** | **N16R2 — quad** | **free** | **35 / 36 / 37** |
> | current rev | N16R8 — octal | **taken by PSRAM** | 18 / 17 / 46 |
>
> ESP32-S3 octal PSRAM occupies GPIO 35/36/37. The first revision's quad part leaves them free,
> so the LCD used them; when Makerfabs moved to the octal N16R8 they had to relocate the LCD.
> Both maps are published by Makerfabs, for different boards, with nothing marking which is which.
>
> **Sources — check the revision, not just the vendor:**
>
> - **rev 1 (this unit) — pinned locally:**
>   [`references/makerfabs-parallel-tft-lvgl-lgfx`](../../references/makerfabs-parallel-tft-lvgl-lgfx/main/LGFX_MakerFabs_Parallel_S3.hpp)
>   (`radiosound-com/makerfabs-parallel-tft-lvgl-lgfx` @ `6d4b014`) — WR 35, RS 36, CS 37, RD 48,
>   40 MHz. This is the **source of record** for this board.
> - **current rev — NOT pinned, and do not follow it:**
>   [Makerfabs' `IDF/matouch/.../3-5-ili9488-ft6236/config.h`](https://github.com/Makerfabs/Makerfabs-ESP32-S3-Parallel-TFT-with-Touch/blob/7670a17/IDF/matouch/main/boards/3-5-ili9488-ft6236/config.h)
>   @ `7670a17` — WR 18, DC 17, CS 46. **The map that does not work here.** Its LVGL example builds
>   and boots on this unit and still shows nothing. (Kept as a URL, not a 323 MB submodule: its only
>   job is to be the wrong answer.)
>
> ⚠️ **Makerfabs updated `firmware/SD16_3.5/SD16_3.5.ino` in place** when they revised the board — it
> now reads WR=18/RS=17. Same repo, same path, different hardware, nothing marking the change. Do not
> cite it for rev-1 pins.
>
> **Other facts, verified on hardware 2026-07-16:**
>
> - **RD (GPIO 48) must be driven HIGH** — `esp_lcd`'s i80 driver never touches it; the vendor's
>   examples set it explicitly. `components/ili9488` does this via `pin_rd`.
> - **RST is `NC`** — tied to the board reset; there is no panel reset line to drive.
> - **pclk 40 MHz** works (the rev-1 reference's value). The data bus D0–D15 is **identical across
>   both revisions** — only WR/DC/CS moved.
> - **The driver is LovyanGFX**, not `esp_lcd` — see [`components/ili9488`](../../components/ili9488).
>   `esp_lcd` i80 was never made to work on this board, though that was tried before the pin map was
>   understood, so it is untested on the correct pins.

---

## Board 2 — ESP32-S3 Parallel TFT with Touch 4.0" (ST7701), 480×480 — **later target**

- **Module (SKU E32S3RGB40):** ESP32-S3-WROOM-1-**N16R8** — 16 MB flash, **8 MB octal PSRAM**.
- **Display:** ST7701S, 480×480, **RGB565 (16-bit) parallel + 3-wire SPI** for init. **No panel
  GRAM → continuous PSRAM scanout** (the Gate #5 drift risk). Shipped PCLK 14 MHz → ~49 Hz.
- **⚠ Backlight:** not PWM-dimmable as shipped — needs a solder mod (add R28 = 1 kΩ, remove R29).
- **Touch:** GT911 capacitive, I²C SDA=17 / SCL=18, RST=38.
- **Audio:** onboard **MAX98357A** I²S amp + speaker — BCLK=20, LRCK=46, DOUT=19.
- **microSD:** SPI — SCK=12, MISO=13, MOSI=11, CS=10 (shared with the 3-wire display SPI).
- **Power:** onboard LiPo charger + connector.

| Signal | GPIO |
|--------|------|
| RGB R0–R4 | 39, 40, 41, 42, 2 |
| RGB G0–G5 | 0, 9, 14, 47, 48, 3 |
| RGB B0–B4 | 6, 7, 15, 16, 8 |
| DE / VSYNC / HSYNC / PCLK | 45 / 4 / 5 / 21 |
| 3-wire SPI CS / SCLK / MOSI | 1 / 12 / 11 |
| Audio BCLK / LRCK / DOUT | 20 / 46 / 19 |
| Touch I²C SDA / SCL / RST | 17 / 18 / 38 |
| microSD SCK / MISO / MOSI / CS | 12 / 13 / 11 / 10 |

**No board config yet** — `boards/makerfabs-st7701-4in/` was removed as premature: this board is a
Phase-2 target that must clear **Gate #5** (the RGB drift soak test) before it is trusted, and an
unused sdkconfig invites someone to build against it. Recreate it when Gate #5 is actually run.

---

## Verification camera

**M5Stack Timer Camera F** (ESP32-D0WDQ6-V3 + OV3660, fisheye, 8 MB PSRAM) — aimed at the panel
for hardware-in-the-loop display verification (per [`.ai/AGENTS.md`](../../.ai/AGENTS.md) →
*Verifying changes*). Setup, capture loop and gotchas:
[`docs/hardware/pico-e32-bench-camera.md`](../hardware/pico-e32-bench-camera.md).

> Replaced an **Espressif ESP-EYE**, whose OV2640 turned out to be dead: it answered on no pin
> pair, at no address, under no XCLK, with either I2C driver, while the same firmware brought
> the Timer Camera up first try. Worth knowing before buying another (the ESP-EYE is also EOL;
> its modern equivalent is the ESP32-S3-EYE).
