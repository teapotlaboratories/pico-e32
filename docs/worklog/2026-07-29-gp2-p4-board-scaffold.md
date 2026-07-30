# 2026-07-29 — GP-2: ESP32-P4 Guition JC4880P443C board scaffold + proof-of-life

Goal ([`GP-2`](../hardware/pico-e32-guition-jc4880p443c-p4.md)): stand up the new **ESP32-P4** board in
the tree and prove **P4 build → flash → boot → serial** end to end, without regressing the S3 path.
Scope is deliberately narrow: a `boards/guition-jc4880p443c/` scaffold (target + PSRAM + console
config, the `board.h` contract, a `board.cpp` **stub**) and a minimal `pico-e32-p4-hello` app that
logs the chip/flash/PSRAM identity and heartbeats. **No display bring-up** — that is GP-3.

Working on branch `gp2-p4-board-scaffold` (code change → branch + PR per
[[../../.ai/AGENTS.md]]). Tree is on ESP-IDF **v6.0.2** (migration landed at `2b1efbe`), which is what
the P4's MIPI-DSI stack needs.

## Board identity (re-confirmed on the connected unit)

`esptool flash_id` over the native USB-Serial-JTAG link:

```
Chip type:  ESP32-P4 (revision v1.3)
Features:   Dual Core + LP Core, 400MHz
Crystal:    40MHz
MAC:        80:f1:b2:d3:7e:ca
Flash size: 16MB   (manufacturer 0x68, device 0x4018)
USB mode:   USB-Serial/JTAG
```

Matches the board doc (P4, 16 MB flash). Identified read-only via
`/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_80:F1:B2:D3:7E:CA-if00` (→ `/dev/ttyACM0`,
volatile).

## Factory-flash backup (done FIRST, before any write)

Before flashing anything, backed up the full 16 MB factory image so the board can be restored.
**This surfaced a serial-link reliability quirk worth recording:**

- A single continuous `read_flash 0 0x1000000 @460800` **dropped** at ~5.6 MB with
  `A fatal error occurred: Packet content transfer stopped`.
- Chunked 1 MB reads @230400: **14/16 chunks OK**; the two in the **5–7 MB region**
  (`0x500000`, `0x600000`) failed all retries. Same region as the full-read drop.
- But a **64 KB** read at `0x500000` succeeds instantly, and a **4 KB** read too. So the flash region
  is *not* bad — it is **long continuous reads over this USB link that drop**; the failures just tend
  to land in that region as the transfer accumulates. 256 KB reads there are also flaky; **64 KB is
  reliable, 4 KB always works.**
- Resolution: 14 good 1 MB chunks kept as-is; the `0x500000–0x700000` span rebuilt from
  **offset-keyed 64 KB sectors**, escalating a stubborn 64 KB to **16×4 KB** reads (460800→115200),
  so the assembled image stays offset-correct and any true hole is explicit (not silently compacted).
- Three 4 KB pages (`0x565000`, `0x5a0000`, `0x619000`) refused every stub-flasher read at both bauds.
  **The fix that cracked them: the ROM loader (`esptool --no-stub`) @115200 — recovered all three on
  the 2nd attempt.** So the culprit is the **stub flasher over this native USB-Serial-JTAG link**, not
  the flash: `--no-stub` reads are slow but reliable. (The pages held real data — 3945/4055/4086 of
  4096 bytes non-`0xFF` — so the `0xFF` placeholder would have been a genuine loss.) Spliced them in.
- **Result: complete 16,777,216-byte image, sha256 `5e1a3c4dc099bfe2710e19946ce96bfca679f9282d48d6361b16cc1e76f79c69`.**
  Verified by re-reading four offsets (`0x0`, the recovered `0x565000`, both chunk boundaries `0x700000`
  / `0xf80000`) with `--no-stub` and diffing against the assembled image — **all four MATCH.**

**Takeaway for all future P4 flashing on this board: prefer `--no-stub` for reads; the stub flasher
drops long/large transfers over this USB link.** Writes (flash) were fine with the stub (see below).

**Archived** at `~/pico-e32-backups/guition-jc4880p443c-factory-16MB-2026-07-29.bin` (+ `.sha256`), out of
the ephemeral scratchpad — owner-approved this one write to `~/` (AGENTS.md otherwise forbids `~/` writes).
Kept out of the repo (binary). It took real effort (3 pages needed ROM-loader recovery) and is irreplaceable.

## Scaffold created

`boards/guition-jc4880p443c/`:

- **`sdkconfig.defaults`** — `CONFIG_IDF_TARGET=esp32p4`, 16 MB flash, external **HEX** PSRAM
  (`SPIRAM_MODE_HEX` + 200 MHz, the P4 defaults, pinned explicitly), and
  **`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`** — makes the native USB-Serial-JTAG the *primary* console
  (this board's only host wire), so all stdout lands on `/dev/ttyACM*` with a single stdout path (no
  UART/USB-JTAG dual-path stdio-lock surprise like the S3 had). Verified against the IDF Kconfig:
  P4 SPIRAM defaults are HEX/200M (`components/esp_psram/esp32p4/Kconfig.spiram`); console primary
  default is UART0 with USB-JTAG only *secondary* (`components/esp_stdio/Kconfig`), hence the override.
- **`board.h`** — the same board-agnostic contract the S3 board exposes, trimmed to the LCD core
  (`board_lcd_init/blit/fill/rgb565`) + geometry (`480×800`). Deliberately defines **neither**
  `BOARD_HAS_SD` nor `BOARD_HAS_TOUCH` yet, so an app's SD/touch paths compile out.
- **`board.cpp`** — **stub**: `board_lcd_init` logs + returns `ESP_ERR_NOT_SUPPORTED`; blit/fill
  no-op; `board_lcd_rgb565` is a live pure function (standard LE RGB565 — the DSI byte order is a
  GP-3 decision, unlike the S3's pre-swap-for-parallel-bus). Header carries the "no unconfirmed pin"
  warning.
- **`README.md`** — per-board README matching the S3 board's format.

## Proof-of-life app

`firmware/pico-e32-p4-hello/` — minimal, board-agnostic. `app_main` logs IDF version, `esp_chip_info`
(target/cores/silicon rev), flash size, PSRAM (`esp_psram_get_size` + initialized flag), internal/PSRAM
heap free, then calls `board_lcd_init()` (stub) and prints the panel geometry, then heartbeats every 1 s
(uptime + free heap). `REQUIRES esp_psram esp_timer spi_flash`; compiles `${BOARD_DIR}/board.cpp` like
the other apps.

## The two build/flash breakages fixed (both real P4/board facts)

1. **`esp_chip_info_t` has no `full_revision`** — the field is `.revision` (format `MXX` =
   major*100+minor). Fixed `main.cpp` (`chip.revision / 100`.`% 100`). Trivial.
2. **`EXTRA_COMPONENT_DIRS = components/` dragged the whole tree into the build.** The `main` component
   *implicitly depends on every discovered component*, so pointing at `components/` pulled in z8lua,
   LovyanGFX, **and `input`** — whose `input_stub.c:14` trips `-Werror=misleading-indentation` under
   this build. This minimal app needs no repo component (board.cpp compiles straight into main), so I
   **removed the `EXTRA_COMPONENT_DIRS` line**. (Noted: `input/input_stub.c` has a latent misindentation
   that will bite GP-3+ if the P4 board later builds `input`; not fixed here — out of GP-2 scope.)
3. **THE big board fact — early v1.3 silicon.** First flash was rejected:
   *"'bootloader.bin' requires chip revision in range [v3.1 - v3.99] (this chip is revision v1.3)."*
   IDF v6 defaults the min supported P4 revision to **v3.1** (`ESP32P4_REV_MIN_301`). This unit is
   **early v1.x silicon**. The IDF Kconfig marks rev **<3.0 and ≥3.0 support as MUTUALLY EXCLUSIVE**,
   "huge hardware difference", ≥3.0 "not compatible with 0.x and 1.x". Fixed in the board
   `sdkconfig.defaults`: `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` + `CONFIG_ESP32P4_REV_MIN_100=y` (v1.0,
   the highest option that still includes v1.3). **Implication for GP-3: v3.x P4 reference code (DSI,
   PSRAM timing) may not apply unchanged — this is early silicon.**

## Build → flash → boot → serial — ✅ VERIFIED

- **Build:** clean on esp32p4. Bootloader `0x5af0`, app `pico_e32_p4_hello.bin` `0x2e550` (~189 KB),
  82 % of the 1 MB app partition free.
- **Flash:** `make flash … BOARD=guition-jc4880p443c PORT=…if00 BAUD=230400` — wrote 189,776 bytes,
  **"Hash of data verified."** The **stub flasher was fine for writes** (only reads were flaky).
- **Boot + run (serial over USB-Serial-JTAG):**
  ```
  I (10) boot: chip revision: v1.3
  I (11) boot.esp32p4: SPI Flash Size : 16MB
  I (59) hex_psram: vendor id : 0x0d (AP) ... density 0x07 (256 Mbit) ... X16 Mode
  I (236) esp_psram: Found 32MB PSRAM device   Speed: 200MHz
  I (1235) app_init: Project name: pico_e32_p4_hello   ESP-IDF: v6.0.2
  I (1235) efuse_init: Min chip rev: v1.0   Max chip rev: v1.99   Chip rev: v1.3
  I (1238) esp_psram: Adding pool of 32768K of PSRAM memory to heap allocator
  I (1242) p4-hello: target=esp32p4  cores=2  silicon_rev=v1.3
  I (1242) p4-hello: flash: 16 MB
  I (1242) p4-hello: PSRAM: initialized=1  size=33554432 bytes (32 MB)
  I (1242) p4-hello: heap free: internal=598355 B  PSRAM=33551752 B
  W (1242) board.p4: board_lcd_init: STUB (GP-2). ... display bring-up is GP-3 — not yet implemented.
  I (1242) p4-hello: board_lcd_init -> ESP_ERR_NOT_SUPPORTED   (panel 480x800)
  I (1242) p4-hello: alive #0 ... I (10242) p4-hello: alive #9  uptime=8s  free_heap=34118076 B
  ```
- **Acceptance met:** P4 build → flash → boot proven end to end; console on native USB-Serial-JTAG works.
  **Bonus, both now confirmed (were 🟡 in the board doc): 32 MB PSRAM present + initialized @200 MHz,
  and silicon rev v1.3.** PSRAM vendor is `0x0d (AP)` = AP Memory, 256 Mbit die ×16, X16 (HEX) mode.
- **S3 non-regression:** the P4 board is purely additive — no shared file touched; the S3 fake08 path is
  unchanged (its own build was already green on v6.0.2 per `2b1efbe`). *(Not re-run this session; the
  change set adds `boards/guition-jc4880p443c/` + `firmware/pico-e32-p4-hello/` only.)*

## Board state

The P4 board currently holds `pico-e32-p4-hello` — a known-good idle state (it only logs + heartbeats,
never touches hardware). Factory image is backed up (see above) should a restore be needed.

## Open / next

- Factory backup archived to `~/pico-e32-backups/` (done) — verified sha256.
- Land: branch `gp2-p4-board-scaffold` → PR → `/review` → rebase-merge (commit held until after
  17:00 Pacific per the work-hours rule). **Owner opted to start GP-3 before landing GP-2** (2026-07-29).
- Then **GP-3**: real ST7701S/MIPI-DSI bring-up — needs the board's schematic/demo package for the init
  sequence + reset/backlight GPIOs + DSI timing (still unverified), and must account for this being
  **early v1.x P4 silicon** (v3.x references may diverge).
