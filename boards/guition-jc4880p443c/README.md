# guition-jc4880p443c

**Guition JC4880P443C-I-W — 4.3″ 480×800 IPS smart display, ESP32-P4 (RISC-V).**

The project's **second, different-architecture** target, alongside the ESP32-S3 boards. Selecting it
(`BOARD=guition-jc4880p443c`) pulls in this directory's `board.{h,cpp}` and `sdkconfig.defaults`
(target=esp32p4, 16 MB flash, HEX PSRAM, USB-Serial-JTAG console). Full bring-up plan + backlog:
[`docs/hardware/pico-e32-guition-jc4880p443c-p4.md`](../../docs/hardware/pico-e32-guition-jc4880p443c-p4.md).

> 🚧 **SCAFFOLD (GP-2).** `board.cpp` is a proof-of-life **stub**: the display entry points log and
> no-op, so a minimal app can prove **P4 build → flash → boot → serial** end to end. The real display
> bring-up (ST7701S over MIPI-DSI, PSRAM framebuffer, rotation quirk) is **GP-3**; GT911 touch and
> ES8311 audio come after. **No pin map is committed here yet** — the DSI timing, reset/backlight
> GPIOs, and ST7701S init sequence are unverified until checked against this board's own
> schematic/demo package (the same "trust the schematic, not a plausible source" caution that cost
> two days on the S3 rev-1 pin map). Do not hardcode a plausible-but-unconfirmed pin.

## What this board is

| | detail | confidence |
|---|---|---|
| MCU | **ESP32-P4** rev v1.3 (RISC-V dual-core + LP core, 400 MHz; **no native WiFi/BT**) | ✅ esptool |
| Radio | ESP32-C6 companion (off the display path) | 🟡 vendor |
| Flash | **16 MB** | ✅ esptool |
| PSRAM | 32 MB HEX (vendor spec) — **size verified at boot by `pico-e32-p4-hello`** | 🟡→ verifying |
| Display | 4.3″ IPS **480×800**, **ST7701S** over **MIPI-DSI** | 🟡 multi-source |
| Touch | **GT911** capacitive, I²C | 🟡 multi-source |
| Audio | **ES8311** codec, I²C | 🟡 multi-source |
| USB | native **USB-Serial-JTAG** (`303a:1001`), MAC `80:f1:b2:d3:7e:ca` | ✅ observed |

**Identify the board by a stable label, not a `/dev/tty*` port.** This is the only native-USB device
on the bench — match it read-only via
`/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_80:F1:B2:D3:7E:CA-if00` (currently
`/dev/ttyACM0`, but treat that number as volatile).

## Config (`sdkconfig.defaults`)

`CONFIG_IDF_TARGET=esp32p4`, 16 MB flash, external **HEX** PSRAM (200 MHz — the P4 default), and the
console pinned to the **native USB-Serial-JTAG** peripheral (the only host wire this board exposes),
so all log/stdout lands on `/dev/ttyACM*` with a single stdout path.

## Files

- `sdkconfig.defaults` — target + flash + PSRAM + console config for this board.
- `board.h` — the board-agnostic contract apps draw through (`board_lcd_*` + geometry macros). Neither
  `BOARD_HAS_SD` nor `BOARD_HAS_TOUCH` is defined yet, so those app paths compile out.
- `board.cpp` — **GP-2 stub**. Real MIPI-DSI/ST7701S bring-up lands here in GP-3.

## Build / flash

```
make flash-monitor APP=pico-e32-p4-hello BOARD=guition-jc4880p443c PORT=/dev/ttyACM0
```

`pico-e32-p4-hello` is the minimal proof-of-life app: it logs chip/flash/PSRAM identity, calls the
board display seam (stub), and heartbeats. Flashing/monitoring is over the native USB-Serial-JTAG.

## See also

- [`docs/hardware/pico-e32-guition-jc4880p443c-p4.md`](../../docs/hardware/pico-e32-guition-jc4880p443c-p4.md) — bring-up plan + `GP-*` backlog + reference ST7701S implementations.
- The repo `Makefile` — how `BOARD=<name>` selects the pin map + `sdkconfig`.
