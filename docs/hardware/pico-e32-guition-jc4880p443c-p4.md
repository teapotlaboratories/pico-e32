# Guition JC4880P443C-I-W (ESP32-P4) — board bring-up

Supporting a **second, different-architecture** target: the Guition JC4880P443C-I-W, a 4.3″ 480×800
IPS smart-display built on **ESP32-P4** (RISC-V), alongside the existing ESP32-S3 boards. The goal set
with the owner (2026-07-28): **display bring-up first**, and **keep the codebase portable across both
ESP32-S3 and ESP32-P4** — no S3 regression.

Backlog IDs below are `GP-*` (Guition-P4), mirroring the `DP-*` (display) / `BC-*` (bench-cam) style.

## What this board actually is

The model name decodes as **JC-4880-P4-43C** → 480×800, ESP32-**P4**, 4.3″. It is **not** an ESP32-S3
(the assumption the model number invites). Confirmed three ways: esptool on the connected unit reads
`Chip is ESP32-P4 (revision v1.3)`, the vendor spec says P4, and the name decodes to P4.

| | detail | confidence |
|---|---|---|
| MCU | **ESP32-P4** rev **v1.3 — EARLY silicon** (RISC-V dual-core 360 MHz, **no native WiFi/BT**) | ✅ esptool + boot |
| Radio | **ESP32-C6** companion (SDIO/esp-hosted) — for WiFi/BT, off the display path | 🟡 vendor/CNX |
| Flash | **16 MB** (AP/Boya generic) | ✅ esptool + boot |
| PSRAM | **32 MB, HEX/X16, 200 MHz — AP Memory** (vendor id `0x0d`, 256 Mbit die) | ✅ **verified at boot (GP-2)** |
| Display | 4.3″ IPS **480×800**, **ST7701S** over **MIPI-DSI** (2-lane @ 500 Mbps, 34 MHz DPI) | ✅ **lit + verified (GP-3/4)** |
| Touch | **GT911** capacitive, I²C | 🟡 multi-source |
| Audio | **ES8311** codec, I²C (bonus — the S3 project's Gate-4 audio gap) | 🟡 multi-source |
| USB | native USB-Serial-JTAG (`303a:1001`), MAC `80:f1:b2:d3:7e:ca` | ✅ observed |

**Identify the board by a stable label, not a `/dev/tty*` port** (the S3 board and this one renumber on
replug). This P4 is the only native-USB device on the bench, so match it read-only via
`/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_80F1B2D37ECA-*` (or the MAC `80:f1:b2:d3:7e:ca`)
— it currently maps to `/dev/ttyACM0`, but treat that number as volatile. Flash through the `make` wrapper
(`BOARD=guition-jc4880p443c`), never raw `idf.py`, so the board's `sdkconfig.defaults` (target=esp32p4, PSRAM,
flash) is layered in.

> ⚠️ **This is EARLY v1.3 P4 silicon — it needs a special build, and it constrains GP-3.** IDF v6
> defaults the minimum supported P4 revision to **v3.1**, so a stock build's bootloader **refuses to run**
> (*"requires chip revision [v3.1 - v3.99], this chip is v1.3"*). The board `sdkconfig.defaults` opts into
> the <3.0 branch: `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` + `CONFIG_ESP32P4_REV_MIN_100=y` (min v1.0, the
> highest that still includes v1.3). IDF marks rev **<3.0 vs ≥3.0 support as mutually exclusive** with
> "huge hardware difference" — so **v3.x P4 reference code (DSI init, PSRAM timing) may not apply unchanged
> in GP-3**; treat this as early silicon.
>
> **Flashing quirk on this native USB-Serial-JTAG link:** the **stub flasher drops long/large `read_flash`
> transfers** (fails mid-read, "Packet content transfer stopped"), but **writes/flash are fine**. For reads
> (e.g. a flash backup), use the **ROM loader — `esptool --no-stub`** (slower but reliable), or keep reads
> ≤64 KB. This is how the factory backup was completed. See
> [`../worklog/2026-07-29-gp2-p4-board-scaffold.md`](../worklog/2026-07-29-gp2-p4-board-scaffold.md).

### Display config (GP-3) — ✅ HARDWARE-VERIFIED on this board (2026-07-29)

Sourced from **ESPHome's board-specific model** for this exact panel
([esphome/esphome#12068](https://github.com/esphome/esphome/pull/12068),
`components/mipi_dsi/models/guition.py`, model `"JC4880P443"`) and then **confirmed on the glass** — the
panel lit on the first flash with correct colour + orientation (GP-3/GP-4 worklog). So the values below are
no longer just a candidate; they are the working config. (The vendor schematic package is still a nice
second source for the audio/touch pins and any gamma polish, but is off the critical path.) Note this is
**early v1.x silicon** and the DSI path worked as-is — no v3.x-vs-v1.x divergence hit the display.

| param | value | source |
|---|---|---|
| Controller / res | ST7701(S), **480×800**, color order **RGB** | #12068 |
| DSI | **2 lanes @ 500 Mbps**, DPI pixel clock **34 MHz** | #12068 |
| H timing | pulse **12**, back porch **42**, front porch **42** | #12068 |
| V timing | pulse **2**, back porch **8**, front porch **166** | #12068 |
| **LCD reset** | **GPIO5** | #12068 (mipi_dsi model) |
| **Backlight** | **GPIO23** (LEDC PWM) | ESPHomeDesigner #254 + search |
| DPHY power | internal LDO **channel 3 @ 2500 mV** | IDF P4 fixed (`esp_lcd/dsi` test board) |
| GT911 touch | I²C **SDA GPIO7 / SCL GPIO8**, **reset GPIO3**, 400 kHz | ESPHomeDesigner #254 |
| ES8311 audio | addr **0x18**, I²C SDA7/SCL8, I2S MCLK13 / BCLK12 / WS(LRCLK)10 / DOUT9 / DIN48, **PA-enable GPIO11 (active-high)** | ESPHomeDesigner #254 + giltal/RetroESP32-P4 |
| **TF/microSD — wiring** | slot-0 IO_MUX pins: **CLK GPIO43 / CMD GPIO44 / D0 GPIO39 / D1 GPIO40 / D2 GPIO41 / D3 GPIO42**; card 3.3 V rail from **on-chip LDO channel VO4** (`ldo_chan_id=4`, 3300 mV) — the rail must be powered **before** any mount attempt in *either* mode, else every init command times out (`send_op_cond 0x107`), which looks exactly like a dead slot | giltal/RetroESP32-P4 `odroid_sdcard.c` + IDF P4 defaults |
| **TF/microSD — how we drive it** | **SDMMC slot-0, 4-bit @40 MHz** (`BOARD_HAS_SDMMC` + `board_sd_mount`) — the fast path: measured **10.20 MB/s** vs **1.43 MB/s** for the same card over SPI, which is **−38% on a cover-art load** (64.0 → 39.4 ms median). Pins as the row above. The P4 has **one** SDMMC host and the on-board **ESP32-C6 radio** needs it too (slot 1, esp-hosted/SDIO); the two cannot be initialised at once, but the host **can be handed over at runtime** — `board_sd_unmount()` releases it before a WiFi session and `board_sd_mount()` takes it back after (remount ~44 ms). That handover is why the radio is on-demand (`WC-5`). ⚠️ **Do NOT call `sdmmc_host_deinit()` after `esp_vfs_fat_sdcard_unmount()`** — the unmount already deinits the host and frees the card (IDF `vfs_fat_sdmmc.c: unmount_card_core`), so a second call is a double-deinit that **boot-loops the board**. The LDO VO4 rail stays powered across the handover. | IDF P4 SDMMC defaults + giltal/RetroESP32-P4 (`board_sd_mount`/`board_sd_unmount` in `board.cpp`); measured `WC-6` |
| **ESP32-C6 radio (WiFi)** | SDIO **CLK GPIO18 / CMD GPIO19 / D0–D3 GPIO14–17**, **C6 reset GPIO54**, C6 boot **IO9**; P4↔C6 UART GPIO29/30. Matches esp-hosted's own P4 defaults. C6 ships with esp-hosted **slave firmware 2.12.12** | GustavoH-Smart/esp32p4 `README_WIFI` + CNX P4+C6 writeup + buccaneer-jak/JC4880P443C-…RS232 (`BOARD_HAS_WIFI` in `board.h`) |

The full 37-command ST7701 init sequence is captured verbatim in the GP-3 worklog. **Correction to the
earlier note:** the "reset GPIO23 / backlight GPIO22" source was **wrong for this board** — LCD reset is
**GPIO5**, GPIO3 is the *touch* reset, GPIO23 is backlight. Exactly the "trust the schematic, not a
plausible source" trap. The **authoritative** source remains the vendor package
`JC4880P443C_I_W.zip` (pin map + schematics + Arduino ST7701S demo), linked from
[MiniWebRadio #791](https://github.com/schreibfaul1/ESP32-MiniWebRadio/issues/791) — its direct link now
serves an HTML portal, so it needs manual retrieval.

**Feasibility (GP-3) — checked in IDF v6.0.2:** MIPI-DSI is viable here — `esp_lcd/dsi` (bus/DBI/DPI) +
the P4 DPHY LL are present, and there is **no chip-revision guard** in the DSI path. DPHY power is the
LDO channel-3 @ 2500 mV above. The one thing only first light will settle: whether the DPHY on **early
v1.x** silicon behaves like the v3.x parts these references were tuned on.

## Portability principle (the hard constraint)

The existing design already supports multiple targets — **`boards/<name>/sdkconfig.defaults` owns
`CONFIG_IDF_TARGET`**, and apps/components draw through the board-agnostic `board.h` contract
(`board_lcd_*`, `board_sd_config`, `board_touch_*`) without naming a chip. The P4 board is a new
`boards/guition-jc4880p443c/` implementing the **same `board.h`** via `esp_lcd_mipi_dsi` + ST7701S, while
the S3 board keeps its LovyanGFX i80 path. Known portability blockers to fix so a P4 build is clean and
S3 is untouched:

- **`make install` was hardcoded `esp32s3`** — ✅ **fixed (`GP-1`)**: now installs `esp32,esp32s3,esp32p4`
  (configurable via `IDF_TARGETS`). The riscv32 compiler P4 needs was already present; `make install
  IDF_TARGETS=esp32p4` verified clean (only the riscv32 GDB was pulled).
- **LovyanGFX** — *not* a blocker after all: its `esp-idf.cmake` globs **all** platforms (incl.
  `platforms/esp32p4/*.cpp`) and already has an IDF-v6 `REQUIRES` branch; each platform `.cpp` is
  `#if CONFIG_IDF_TARGET_*`-guarded, so it compiles clean (empty) on non-matching targets. And a P4 board
  driving the panel via `esp_lcd_mipi_dsi` won't `REQUIRES` it anyway. (The earlier "S3-only" note was a
  misread of a targeted grep that only surfaced the esp32s3 line.)
- z8lua's `-fjump-tables -ftree-switch-conversion` is a **generic GCC flag** (portable to RISC-V). **P4
  throughput measured (2026-07-31): Celeste runs at a steady 60 Hz loop (30 fps drawn) with ~9.6 ms/frame
  headroom on core 0 while audio synth runs on core 1 — Gate #4 met.** (Earlier "Xtensa-only / deferred"
  note is obsolete.)

## Plan (staged, Gate-style) — display first

| # | phase | status |
|---|---|---|
| **GP-1** | **Portability foundation** — ✅ **DONE**: `make install` now covers `esp32p4` (via `IDF_TARGETS`), P4-toolchain install verified. LovyanGFX needed no change (already multi-targets incl. p4); S3 build path untouched (`install` ≠ `build`). | ✅ |
| **GP-2** | **P4 board scaffold + proof-of-life** — ✅ **DONE (2026-07-29).** `boards/guition-jc4880p443c/` (`sdkconfig.defaults` target=esp32p4 + 16 MB flash + HEX PSRAM + USB-Serial-JTAG console + **v1.x rev-min**, `board.h` 480×800, `board.cpp` stub) + `firmware/pico-e32-p4-hello/` (chip/PSRAM dump + heartbeat). **Build → flash → boot proven on serial**; factory flash backed up first. **Bonus: 32 MB PSRAM + rev v1.3 confirmed at boot.** See [worklog](../worklog/2026-07-29-gp2-p4-board-scaffold.md). | ✅ |
| **GP-3** | **Display bring-up (the core)** — ✅ **DONE (2026-07-29).** `board.cpp` implements `board_lcd_*` via IDF's built-in `esp_lcd/dsi`: LDO ch3 → DSI bus (2×500 Mbps) → DBI + hand-rolled ST7701 37-cmd init → DPI (34 MHz) + PSRAM framebuffer; reset G5, backlight G23. **First light on the first flash**, zero init errors on early v1.x silicon. The ST7701S-on-DSI rotation quirk turned out **not needed** (native-portrait renders upright). See [worklog](../worklog/2026-07-29-gp3-p4-display-research.md). | ✅ |
| **GP-4** | **Gate-1 verification on the real glass** — ✅ **DONE (2026-07-29).** RGB fills + four corner markers on the bench camera: **RED-TL · GREEN-TR · BLUE-BL · WHITE-BR** exactly as drawn → **colour order correct (no BGR swap), orientation upright**. The panel confirmed it, not a checksum. | ✅ |
| **GP-5** | **Input (serial backend + GT911 touch)** — ✅ **DONE (2026-07-30).** Two input paths on the same `input.h` seam, both hardware-verified. **(a) Serial input — ✅ VERIFIED:** ported the S3's `input_serial` backend; its transport is now console-config-conditional (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` → native USB-Serial-JTAG on P4, else UART0 on S3 — one file, S3 unchanged). `pico-e32-p4-hello` drives an on-panel marker from `l/r/u/d`/`z`/`x`; injecting keys over USB-JTAG produced exact held masks + marker moves (`held=0x02` right, `0x08` down, `0x10` O, `0x20` X, `0x01` left) — the full host→backend→app path works. **(b) GT911 touch — ✅ VERIFIED on hardware (2026-07-30):** `board_touch_init`/`board_touch_read` (v6 `i2c_master`, 16-bit regs) + `BOARD_HAS_TOUCH`; controller `GT911 id "911" @ 0x5D, SDA7/SCL8`. **Owner tapped the panel — all buttons register correctly**, so the GT911 read + zone mapping are confirmed (closes the GP-5b HITL gap). The approved control deck (d-pad + O/X + MENU, `pico-e32-fake08-touch-ui.html`) renders on the P4 via a **portable** `input_touch.c`: geometry-derived zones (reproduce the S3's 320×480 layout exactly, scale to 480×800) + a deck renderer through `board_lcd_blit` (S3 LovyanGFX deck removed; added `board_lcd_width/height` to the seam), with gradient d-pad bars (rounded) + chevrons + spherical O/X buttons matching the mockup. | ✅ |
| **GP-6** | **ES8311 audio** — ✅ **DONE (2026-07-31).** Onboard codec via the vendored `espressif/esp_codec_dev`, over the shared touch I²C bus (**0x18**, SDA7/SCL8) + I2S (**MCLK13 / BCLK12 / WS10 / DOUT9**), 22050 Hz S16 stereo. Wired to fake-08's audio poll path through the `board_audio_*` seam (`board.cpp`); S3 is a no-op stub (no HW). **Key gotcha: the speaker power-amp has an enable pin, GPIO11 (active-high)** — without it the DAC plays but nothing is audible (found via giltal/RetroESP32-P4). **HITL-verified: Celeste's title music plays loud + clear** (mic capture −17 dB w/ full-scale peaks; spectrogram shows chiptune structure). Isolated with a standalone tone app `firmware/pico-e32-p4-audio`. | ✅ |
| **GP-8** | **SD cart launcher + cover-art carousel** — ✅ **DONE (2026-08-03).** Boot into a browser of the microSD's PICO-8 library, launch a cart by selecting it. **P4 SD** — the blocker was **power, not pins**: the card rail is fed by on-chip **LDO VO4**, which must be enabled before mount (else `send_op_cond 0x107`); fix from giltal/RetroESP32-P4. *(Briefly moved to SPI for `GP-9`/`WC-3` so the always-on radio could hold the SDMMC host; **restored to SDMMC by `GP-10`/`WC-6`** once the radio became on-demand. The LDO requirement is unchanged either way.)* Native **cover-art carousel** (`firmware/pico-e32-fake08/main/carousel_launcher.cpp`, `-D LAUNCHER=1`): lodepng-decodes each `.p8.png` label, browses the SD folder tree (needed only the esp32 `Host::listdirs()`), launches into the VM. **Panel-agnostic** — layout from a new board seam `board_carousel_layout()` (P4 game 384@x48, thumb 236×302; S3 game 256@x32, thumb 148×190), so the **same firmware runs on the S3** (confirmed live, `BOARD=makerfabs-ili9488-r1`). Camera-verified on both; **playtested** a racing cart end-to-end (carousel → launch → drive → game over) at **~30 fps** — which surfaced + fixed an `SHOW_FPS` HUD bug (repainted only on value change, so the P4's full-width game overwrote it → now repaints per tick). Dead end recorded: framebuffer-over-serial (`FB_DUMP`) unreliable on the P4 USB-JTAG. See [worklog](../worklog/2026-08-03-p4-sd-launcher-carousel.md). | ✅ |
| **GP-7** | **Feature parity — fake-08 runtime on P4** — ✅ **CORE DONE (2026-07-29).** `pico-e32-fake08` builds + runs for `BOARD=guition-jc4880p443c`: z8lua + fake-08 on **RISC-V**, **real Celeste renders on the P4 panel** (title → gameplay room) driven over serial input — camera-verified. Made the host geometry-agnostic (runtime `OX/OY` from the panel size via the Host ctor; auto-centred 256×256 on 480×800; S3 unchanged). **Fixed a RISC-V-only z8lua bug** (see below). **Gate #2 re-validated on RISC-V:** Celeste computes at **~8 ms/frame** (Step ~5.4 + draw ~2.7 ms, ~120 fps headroom, worst-frame ~13–21 ms) — ~2× the S3, comfortably 30/60-capable; gameplay video captured. See [worklog](../worklog/2026-07-29-gp3-p4-display-research.md). | ✅ |
| **GP-9** | **WiFi via the on-board ESP32-C6** (backlog `WC-3`) — ✅ **DONE (2026-08-05).** The P4 has no native radio; `esp_wifi_remote` → **esp-hosted (SDIO)** → the C6 (pins in the table above), behind the same `components/wifi` front-end and launcher **WIFI** menu the S3 uses (`BOARD_HAS_WIFI`). Two P4-only traps, both fixed: **(a)** esp-hosted auto-inits from a **C global constructor before `app_main`**, which hangs this firmware in the C6 SDIO bring-up (boot stops at `H_SDIO_DRV: sdio_data_to_rx_buf_task started`) → deferred to `app_main` (`esp_hosted_init()` + the *blocking* `esp_hosted_connect_to_slave()` in `wifi_mgr_init`), with the constructor neutralised **at build time** from the project CMake (see "esp-hosted local patch" below). **(b)** SD + C6 **share the single SDMMC host** → at the time the SD moved to **SPI**; since `GP-10` it stays on SDMMC and the host is handed over per WiFi session instead (see the pin table). Verified in one boot: `SD mounted at /sdcard` + `Identified slave [esp32c6]` + `wifi: STA up` + launcher rendering; driver-level join proven (IP 192.168.7.212). See [worklog](../worklog/2026-08-05-wifi-connectivity.md). | ✅ |

| **GP-10** | **SD back on SDMMC + runtime host handover** (backlog `WC-6` step 1) — ✅ **DONE (2026-08-06).** With the radio on-demand (`WC-5`) the SDMMC host is free almost always, so the SD reclaims it: **cover-art load 64.0 → 39.4 ms median (−38%)**, read **26.3 → 3.6 ms (7.1×)**; PNG decode is unchanged (~29 ms) and is now ~75% of what remains, so the bus is no longer the bottleneck. The launcher lends the host to the radio for a WiFi session (`board_sd_unmount` → acquire → release → `board_sd_mount`, remount ~44 ms, card verified fully usable after) via a scope guard so no exit path can forget it. Trap: never `sdmmc_host_deinit()` after the VFS unmount — double-deinit, boot loop. Step 2 (SD on SPI *simultaneously* with the radio, for downloads needing both) deferred until `WC-4` has a consumer. See [worklog](../worklog/2026-08-06-p4-sd-sdmmc-handover.md). | ✅ |

## esp-hosted — local patch (build-time, not a vendored source edit)

`espressif/esp_hosted` is pulled by the component manager into the **git-ignored** `managed_components/`, so
it is *not* a vendored component with its own `LOCAL_PATCHES.md`. It nevertheless carries one deliberate
deviation, recorded here so it stays auditable:

- **What:** in `host/port/esp/freertos/src/port_esp_hosted_host_init.c`, the global constructor's
  `ESP_ERROR_CHECK(esp_hosted_init());` is commented out.
- **Why:** running it before `app_main` hangs this firmware during the C6 SDIO bring-up (GP-9a above). The
  init is instead performed from `app_main` via `wifi_mgr_init()` (`components/wifi/wifi_manager.c`).
- **How:** an idempotent `string(REPLACE …)` at configure time in
  [`firmware/pico-e32-fake08/CMakeLists.txt`](../../firmware/pico-e32-fake08/CMakeLists.txt), guarded to the
  `esp32p4` target. The fetched tree therefore stays **byte-identical to upstream** (nothing modified is
  committed) and the transform re-applies automatically after any clean or re-fetch — per the
  "least-destructive vendor edits / push integration into the build" rule in `.ai/AGENTS.md`. If upstream
  moves the call, the build prints a warning instead of silently doing nothing.
- **Upstream version:** `esp_hosted` 2.12.12 (host) / C6 slave firmware 2.12.12; `esp_wifi_remote` 1.6.3.

## Reference implementations (for the ST7701S init + pins)

- Direct board-support project for the JC4880P443C_I_W (manual ST7701S DSI bring-up, PPA hardware-rotate,
  GT911) — the closest match; get the exact init + pins here.
- [`Nicolai-Electronics/esp32-component-st7701`](https://github.com/Nicolai-Electronics/esp32-component-st7701) — reusable ESP32-P4 ST7701 MIPI-DSI component.
- [`embenix/ESP32-P4-DSI-Support-Hub`](https://github.com/embenix/ESP32-P4-DSI-Support-Hub) — DSI panel boilerplate.
- [esp-idf #16201](https://github.com/espressif/esp-idf/issues/16201) — ST7701S MIPI-DSI rotation on P4 (the quirk).
- [MiniWebRadio #791](https://github.com/schreibfaul1/ESP32-MiniWebRadio/issues/791) — this exact board: ST7701 / GT911 / ES8311, links to the Guition package.
- [`giltal/RetroESP32-P4`](https://github.com/giltal/RetroESP32-P4) — retro-emulator project for this P4 board; `components/app_common/include/pins_config.h` + `components/audio/audio.c` are the authoritative **audio** pin map (I2S + **PA-enable GPIO11, active-high**) and a working ES8311/`esp_codec_dev` setup — the source of the GP-6 amp-enable fix.
- [Guition ESP32-P4 module page](https://www.guition.com/esp32p4-display-module/esp32p4-display) · [CNX writeup (P4+C6)](https://www.cnx-software.com/2025/08/12/4-3-inch-touch-display-board-features-single-esp32-p4-esp32-c6-module-supports-camera-and-speakers/)

## Open questions for the owner

1. **Guition demo/schematic package** — if it's available locally, it's the authoritative source for the
   ST7701S init sequence + pin map (beats reverse-engineering from third-party repos). **Now doubly
   important given this is early v1.x silicon** — third-party P4 DSI repos likely target v3.x.
2. **Panel verification (GP-4)** — re-aim the bench camera at the P4 panel, or eyeball the first fills?
3. ~~**Factory backup archival**~~ — RESOLVED (2026-07-29): the verified 16 MB factory image (sha256
   `5e1a3c4d…`) is archived at `~/pico-e32-backups/guition-jc4880p443c-factory-16MB-2026-07-29.bin`.
