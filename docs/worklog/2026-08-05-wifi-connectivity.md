# 2026-08-05 — WiFi connectivity (S3 native + P4 via the ESP32-C6)

Goal ([`WC-1`](../pico-e32-todo.md)/[`WC-3`](../pico-e32-todo.md)): give both handhelds a **WiFi connection menu** —
scan, join with an on-screen keyboard, persist, and auto-reconnect on boot — as the foundation for NTP, OTA, and
network cart downloads. The S3 has a native radio; the P4 has none and drives an on-board **ESP32-C6** over
SDIO/esp-hosted. Built S3 first (per the owner's scoping), then the P4.

## TL;DR

- **One front-end, two backends.** New [`components/wifi`](../../components/wifi) exposes a small STA API
  (`scan / connect / status / save / load / forget / autoconnect`). The component CMake picks the backend by IDF
  target: native `esp_wifi` on the S3, `esp_wifi_remote` → esp-hosted → C6 on the P4. The launcher's Settings →
  **WIFI** submenu (status, scan, deck-driven on-screen keyboard, connect+persist) is identical on both.
- **S3 — done, verified.** Scan → keyboard → connect → NVS-persist → boot auto-reconnect. Joined `Tukang Ketoprak`,
  got 192.168.7.228.
- **P4 — done, verified.** The C6 comes up and the same menu drives it. Two P4-specific traps, both solved:
  1. **esp-hosted's boot auto-init hangs** the P4 before `app_main` → deferred to `app_main` + neutralized the
     constructor at build time.
  2. **SD and C6 fight over the single SDMMC host** → moved the SD to **SPI**, freeing the SDMMC host for the C6.
- **Decisive result:** one P4 boot now mounts the SD *and* identifies the C6 *and* renders the launcher — WiFi +
  MIPI-DSI + SD all coexist.

## 1. The component: one API, backend by target

`components/wifi/wifi_manager.{h,c}` is a thin esp_wifi STA manager: a mutex serialises boot-autoconnect vs. the
menu, an event group turns the async connect into a blocking `wifi_mgr_connect(ssid, pass, timeout)`, and we own
persistence in NVS (`WIFI_STORAGE_RAM` so esp_wifi doesn't double-store). The whole thing is written to the plain
`esp_wifi.h` API — which is exactly why the P4 can reuse it: `esp_wifi_remote` re-implements that same API and
forwards the RPCs to the C6. The only per-target code is:

- **CMakeLists** — `REQUIRES esp_wifi esp_netif esp_event nvs_flash` on the S3; adds
  `esp_wifi_remote esp_hosted` on the P4. (`esp_wifi_remote` re-implements the API but does **not** re-export the
  header, so `esp_wifi` must stay in `REQUIRES` for `esp_wifi.h` to resolve — a build error that looked like a
  missing dependency but is by design.)
- **A short `#if CONFIG_IDF_TARGET_ESP32P4` block in `wifi_mgr_init`** that brings the C6 link up first (see §3).

An earlier design used a no-op `wifi_manager_stub.c` on radio-less targets; that's gone — the P4 has a real backend
now, so the stub was deleted rather than kept as dead weight.

## 2. The menu + on-screen keyboard

Settings → **WIFI** (gated on `BOARD_HAS_WIFI`): shows current status/IP, scans the air into a selectable list,
and on select opens a **deck-driven on-screen keyboard** (navigate with the d-pad, O to type, MENU to submit) for
the password, then connects and persists. Board-driven layout (`board_carousel_layout()`) keeps it correct on both
panels. Known follow-up [`WC-2`](../pico-e32-todo.md): the PICO-8 font is uppercase-only, so a lowercase password
*displays* uppercase (it is **stored** with correct case, so joins work) — lowercase glyphs are a low-priority
font-table add.

## 3. P4 trap #1 — esp-hosted's boot auto-init hangs before `app_main`

esp-hosted initialises itself from a **C global constructor**
(`port_esp_hosted_host_init.c`: `__attribute__((constructor))` → `ESP_ERROR_CHECK(esp_hosted_init())`), i.e. before
`app_main`. In a minimal probe app that's fine; in the full fake-08 firmware it **stalls the P4 in the C6 SDIO
bring-up** — the boot log reaches `H_SDIO_DRV: sdio_data_to_rx_buf_task started` and then dies, `app_main` never
runs (no board/SD/carousel logs).

**Fix — defer the init to `app_main`.** `wifi_mgr_init` (P4 branch) does the bring-up explicitly, after board
bring-up, and it's reliable:

```c
esp_err_t he = esp_hosted_init();              /* starts the transport tasks */
if (he != ESP_OK && he != ESP_ERR_INVALID_STATE) return he;
he = esp_hosted_connect_to_slave();            /* blocking: reset C6 (GPIO54), SDIO init, identify slave */
if (he != ESP_OK) return he;
```

`esp_hosted_init()` alone only starts the transport tasks (async, "link not yet up"); the blocking handshake is
`esp_hosted_connect_to_slave()`, so both are needed.

**Neutralising the constructor cleanly.** esp-hosted has no Kconfig to disable the auto-init, and its
`managed_components/` copy is git-ignored (regenerated from the lockfile). Hand-editing it is neither tracked nor
durable. So we transform that one call **at configure time from the project CMake**
([`firmware/pico-e32-fake08/CMakeLists.txt`](../../firmware/pico-e32-fake08/CMakeLists.txt)): the component manager
fetches esp-hosted **byte-identical to upstream**, and an idempotent `string(REPLACE …)` comments out the
constructor's `esp_hosted_init()` call, re-applying automatically after any clean/re-fetch. This matches the repo's
"keep the vendor tree byte-identical, push integration into the build" rule — no modified vendor copy is committed.

## 4. P4 trap #2 — SD and C6 share one SDMMC host

The P4 has a **single SDMMC host**. The microSD is on slot 0 and the C6's SDIO link is on slot 1 — but one host
can't be initialised twice, so whichever comes up first locks the other out. The earlier SD work
([2026-08-03](2026-08-03-p4-sd-launcher-carousel.md)) mounted the SD over SDMMC; with the C6 in the picture that
directly conflicts (SD-first → C6 handshake fails; C6-first → SD mount returns `ESP_ERR_NOT_FOUND`).

**Fix — drive the SD over SPI instead.** The same TF pins can run in SPI mode (CLK→SCLK, CMD→MOSI, DAT0→MISO,
DAT3→CS), which frees the SDMMC host **entirely** for the C6. This reuses the S3's existing `BOARD_HAS_SD` +
`sdcard_spi` seam — so `main.cpp` needs no P4-specific SD path. The P4's `board_sd_config()` fills the SPI
host/pins and still powers the card rail (the on-chip **LDO VO4** — that power is required in SPI mode too, same as
it was for SDMMC). `BOARD_HAS_SDMMC` and the P4's `board_sd_mount()` were removed.

Pin map (P4 TF slot, SPI mode): `SCLK GPIO43, MOSI GPIO44, MISO GPIO39, CS GPIO42`, rail on LDO VO4 (channel 4,
3.3 V; DPHY already uses channel 3).

## 5. Result — coexistence, one boot

```
board.p4: SD over SPI2: SCLK43 MOSI44 MISO39 CS42, rail on LDO VO4
sdcard_spi: SD mounted at /sdcard                 <- SD works over SPI
carousel: carousel layout: game 48+384, ...       <- launcher up
H_API: ESP-Hosted starting ...                     <- from app_main (after SD), not the constructor
transport: Identified slave [esp32c6]              <- C6 radio linked
wifi: STA up                                        <- esp_wifi STA running
```

The ordering confirms the deferred path: esp-hosted starts *after* the SD mount and carousel, i.e. from the
launcher's `wifi_mgr_init`, not the pre-`app_main` constructor. Launcher framebuffer verified via `FB_DUMP` (P4
compressed `SHTZ`). Driver-level connect was proven earlier with a standalone probe
(`firmware/pico-e32-p4-wifi`, joined `Tukang Ketoprak`, IP 192.168.7.212); nothing on the radio side changed since.

## 6. Review pass — measurements and fixes

Code review of the PR surfaced six issues; all are fixed on the branch, and one was a measurement rather than a
change.

**SD throughput after the move to SPI (measured).** Instrumented `cover_rgba()` (temporarily) to time the
read+decode of a `.p8.png` cover, and scrolled the carousel to force uncached loads:

| SPI clock | cover load (n=9) | result |
|---|---|---|
| **20 MHz** (shipped) | min 49.4 / **median 64.0** / mean 65.9 / max 82.6 ms | works |
| 40 MHz | — | **card init fails**: `sdmmc_enable_hs_mode_and_check: send_csd returned 0x108`, mount returns `ESP_ERR_INVALID_RESPONSE` |

So **20 MHz is the practical ceiling** on this wiring — the TF pins run through the GPIO matrix with no external
pull-ups, and the card won't negotiate at 40 MHz. Two things follow. First, the throughput lost relative to
SDMMC 4-bit can't be bought back by raising the clock. Second, it doesn't matter much: SDMMC *cannot* coexist
with the C6 radio at all, so the comparison is moot — the real choice was "SD over SPI" vs "no WiFi". 64 ms to
reveal a not-yet-cached cover is acceptable for a browse UI, and covers are cached after first view. Worth
revisiting only if a future board adds pull-ups. Also confirmed: a failed mount degrades gracefully
(`continuing without SD`) rather than panicking.

**Behavioural fixes.**

- **P4 boot no longer waits on the radio.** `wifi_mgr_init()` was called inline on the launcher thread, and on
  the P4 it blocks ~2.3 s in the esp-hosted handshake — so the menu appeared that much later on every boot, for
  a feature most sessions never touch. Moved into the existing background task. Measured: deck drawn at
  **1775 ms**, down from **4068 ms**; the C6 still finishes at ~4.0 s, just off the critical path.
- **WiFi no longer dies permanently after 3 drops.** The retry counter served both the modal connect *and* the
  steady-state link, and only reset on `GOT_IP` — so an AP reboot or a walk out of range left the handheld
  offline until a manual reconnect. Split into two policies: bounded retries while `wifi_mgr_connect()` is
  waiting (so the menu still gets a definite answer), and an indefinite slow retry (10 s timer) once a link has
  actually been established. An explicit disconnect/forget stands the keepalive down so it stays off.
- **`wifi_mgr_init()` is now thread-safe, not merely idempotent.** Making boot-init async meant the background
  task and Settings → WIFI could both enter it and each sail past the `s_inited` check, running the whole
  bring-up twice. Guarded with a statically-initialised spinlock; the loser waits for the winner.
- **32-byte SSIDs now work.** `strlcpy` into `wifi_config_t.sta.ssid` (32 bytes, not NUL-terminated on the
  wire) silently dropped the 32nd character, so such networks just failed to join. Now `memcpy` with an
  explicit length; the passphrase is likewise capped at its protocol maximum of 63, and `WIFI_PASS_MAXLEN`
  corrected from 64 to 63.
- **Init failure is reported.** The return value was discarded at both call sites; the WiFi menu now says
  `RADIO UNAVAILABLE` instead of showing a generic offline state.
- **Partial-init retry no longer leaks.** A failure after netif creation left `s_inited` false, so a retry
  would create a second netif and re-register handlers; the one-shot allocations are now guarded.

## 7. Commands run (reproduce)

All builds go through the top-level `make` wrapper (never raw `idf.py` — the board overlay owns
`CONFIG_IDF_TARGET`, PSRAM and flash size).

```sh
# P4 — dev build used for the coexistence test (serial input + framebuffer screenshots)
make flash APP=pico-e32-fake08 BOARD=guition-jc4880p443c PORT=/dev/ttyACM0 \
     DEFS="-D LAUNCHER=1 -D FB_DUMP=1 -D INPUT_BACKEND=serial"

# P4 — shipping touch build (what the board is left on)
make flash APP=pico-e32-fake08 BOARD=guition-jc4880p443c PORT=/dev/ttyACM0 DEFS="-D LAUNCHER=1"

# S3 — WiFi foundation build
make flash APP=pico-e32-fake08 BOARD=makerfabs-ili9488-r1 PORT=/dev/ttyUSB0 DEFS="-D LAUNCHER=1"
```

The configure step prints `-- esp-hosted: deferred boot auto-init out of the C constructor (P4 C6 SDIO).`
when the build-time transform applies — its absence on a P4 build means the patch didn't take.

Boot evidence was captured by pulsing RTS (reset) and reading the console for ~20 s; the P4 screenshot came
from the `FB_DUMP` build's PAUSE-triggered compressed dump (`SHTZ` frame → zlib → 480×800 RGB565).

## 8. Board state

- **P4** — left on the **shipping touch build** (`-D LAUNCHER=1`, no `FB_DUMP`/serial input), reflashed after
  testing and confirmed booting clean (SD mounted, C6 identified, STA up, launcher rendering). Known-good.
- **S3** — on its WiFi foundation build (`-D LAUNCHER=1`), joined and auto-reconnecting.

**Note on verification method:** the panel was verified via the **framebuffer-over-serial screenshot**
(`FB_DUMP`), not the bench camera. That's the stronger evidence here — it's the exact scan-out pixels, with no
camera framing/glare/mirror ambiguity — and it now works on both boards. The camera adds nothing this change
depends on (no new display code); backlight/physical output was confirmed by eye.

## 9. Sources

- **P4↔C6 SDIO pin map + esp-hosted wiring:** GustavoH-Smart/esp32p4 (`README_WIFI`), the CNX Software writeup on the
  P4+C6 module, buccaneer-jak/JC4880P443C-…RS232 (P4↔C6 UART, C6 reset GPIO54, C6 boot IO9, JP1). Confirmed against
  esp-hosted's own P4 defaults.
- **SD-over-SPI on P4 TF pins + LDO VO4 rail:** the P4 SDMMC work from [2026-08-03](2026-08-03-p4-sd-launcher-carousel.md)
  (giltal/RetroESP32-P4) for the rail/LDO; SPI-mode pin mapping is the standard TF-in-SPI wiring.
- **Backends:** Espressif `esp_wifi_remote` (1.6.3) + `esp_hosted` (2.12.12, C6 factory firmware).

## 10. State & next

- **MERGED to `main`** 2026-08-05 as pico-e32 `#30` (rebase, linear; commits held during the weekday 9–5 Pacific
  window and landed after 17:00). Review pass ran on the PR — §6 records what it found and how it was fixed.
  Contents: `components/wifi`, the P4 board SD-over-SPI + `BOARD_HAS_WIFI`, the CMake defer-init transform, the
  launcher WIFI menu + keyboard, the OTA-ready 16 MB partition table, and the two helper firmwares
  (`pico-e32-p4-wifi` probe, `pico-e32-p4-c6-ota` C6 slave OTA tool).
- **Follow-ups:** [`WC-2`](../pico-e32-todo.md) lowercase glyphs; [`WC-4`](../pico-e32-todo.md) NTP / OTA / network
  cart downloads.
