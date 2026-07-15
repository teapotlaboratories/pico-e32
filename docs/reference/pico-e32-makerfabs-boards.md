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
| WR / RD / DC(RS) / CS | 18 / 48 / 17 / 46 (tied low) |
| Backlight | 45 (PWM) |
| Touch I²C SDA / SCL | 38 / 39 |
| microSD CS / MOSI / MISO / CLK | 1 / 2 / 41 / 42 |

Board config: [`boards/makerfabs-ili9488/`](../../boards/makerfabs-ili9488/).

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

Board config: [`boards/makerfabs-st7701-4in/`](../../boards/makerfabs-st7701-4in/).

---

## Verification camera

**Espressif ESP-EYE** — aimed at the panel for hardware-in-the-loop display verification (per
[`.ai/AGENTS.md`](../../.ai/AGENTS.md) → *Verifying changes*). Discontinued/obsolete to buy new
(modern equivalent: ESP32-S3-EYE), but already on hand.
