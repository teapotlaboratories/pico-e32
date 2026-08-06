# 2026-08-06 — WiFi off by default, on-demand only (`WC-5`)

Goal: stop the radio being a permanent resident. It came up on every boot and stayed up for the session, which
costs battery (on the P4 a whole second chip stays powered), memory, and CPU that should belong to the game.
Owner's ask, three stated goals: **P4 loading time, battery life, and all cores for the game while playing.**

Design decisions (owner): **pure on-demand** — no boot connect and no persisted "WiFi on" toggle — and a **full
teardown, on both boards**, not just an RF stop.

## TL;DR

- **Radio is off at boot on both boards**, verified: nothing WiFi-related in either boot log until something asks
  for it. On the P4 the C6 is never reset, never handshaken, and esp-hosted's priority-23 tasks never exist.
- **Refcounted `wifi_mgr_acquire()` / `wifi_mgr_release()`.** The last release tears the stack down completely:
  `esp_wifi_stop` + `esp_wifi_deinit`, handlers unregistered, STA netif destroyed, and on the P4
  `esp_hosted_deinit()` to drop the C6 link. Launching a cart forces it down regardless of refcount.
- **The up/down cycle is repeatable and doesn't leak** — the main risk going in was that `esp_hosted_deinit()`
  wouldn't allow a later re-init. It does. Two full cycles on each board, second bring-up within 48 bytes (P4) of
  the first.
- **Memory back when idle: 126.5 KB (P4), 38.0 KB (S3).**
- **Honest null result: this did NOT improve loading time.** Cover-art load is unchanged at a 64.0 ms median with
  the radio off vs on. The idle radio was not stealing measurable throughput from the SD path. Goal 1 is not met
  by this change; goals 2 and 3 are (see §5 for exactly how far each is substantiated).

## 1. The model

`wifi_mgr_acquire()` brings the stack up and takes a reference; `wifi_mgr_release()` drops one and tears down at
zero. Reference-counted so overlapping users compose — the WiFi settings screen and, later, an OTA check or a
download can each hold one without fighting. `wifi_mgr_shutdown()` forces it down whatever the count.

Who acquires today: **only Settings → WIFI**, for as long as that screen is open. It also runs
`wifi_mgr_autoconnect()` on entry so the screen shows the real state rather than a bare OFFLINE. The `WC-4`
features (NTP, OTA, downloads) are the future consumers — each acquires, transfers, releases.

`wifi_mgr_init()` is gone from the public header; bring-up is now internal and reached only through `acquire`.

## 2. Teardown — what actually gets released

```c
esp_wifi_disconnect(); esp_wifi_stop();
esp_event_handler_instance_unregister(...);   /* before deinit, so no event lands on a dead stack */
esp_wifi_deinit();
esp_netif_destroy_default_wifi(s_netif);
#if CONFIG_IDF_TARGET_ESP32P4
esp_hosted_deinit();                          /* drops the C6 link — the actual power win */
#endif
```

The long-lived primitives (mutex, event group, retry timer) are deliberately **not** destroyed. They are small,
and keeping them removes a whole class of use-after-free race against a concurrent scan/connect. Everything that
guards a one-shot allocation is reset so the next bring-up starts clean.

## 3. Measurements

**Radio up/down cycle, P4** (`radio up` / `radio down` log the free heap):

| event | free heap | note |
|---|---|---|
| acquire #1 | 32,692,900 | bring-up ~2.27 s (esp-hosted handshake + C6 identify) |
| release #1 | 32,819,384 | **+126,484 B recovered** |
| acquire #2 | 32,692,852 | within **48 B** of the first — no leak |
| release #2 | 32,819,372 | +126,520 B |

**Radio up/down cycle, S3:**

| event | free heap | note |
|---|---|---|
| acquire #1 | 1,343,092 | bring-up ~0.13 s; autoconnect joined, IP 192.168.7.228 |
| release #1 | 1,381,108 | **+38,016 B recovered** |
| acquire #2 | 1,342,108 | |
| release #2 | 1,381,092 | down-heap within 16 B of the first cycle |

**Cover-art load on the P4 — the loading-time question**, same folder and method both runs (scroll the carousel
to force uncached loads):

| radio | n | min | median | mean | max |
|---|---|---|---|---|---|
| **ON** (connected, previous build) | 9 | 49.4 | **64.0** | 65.9 | 82.6 ms |
| **OFF** (this build) | 10 | 49.9 | **64.0** | 66.2 | 82.4 ms |

**No difference.** The cover path is SPI-read + PNG-decode bound and the idle radio wasn't contending with it.

## 4. Two bugs found while testing

**`esp_hosted` re-init looked broken and wasn't.** The first cycle test showed acquire #2 producing no log at all,
which read exactly like `esp_hosted_init()` hanging after a `deinit` — the risk called out in the `WC-5` plan.
Tracing it (a log at each step of the bring-up state machine, plus one at each screen's entry) showed the
bring-up was never being *reached*: `run_wifi` was never called the second time. Worth the trace — the workaround
for the imagined bug would have been to skip `esp_hosted_deinit()` entirely and give up most of the power saving.

**The actual bug: X backed out two screens.** The serial input backend holds a tap for 6 frames (and a finger on
the deck rests longer than that), so the `X` that closed the WiFi screen was *still asserted* when
`run_settings`' loop polled next. Its `prev` was stale, so it read a fresh X edge and exited Settings too. Fixed
by re-seeding `prev = input_poll()` after a submenu returns — the same pattern already used when entering a
screen. Fixed in both places it can happen (`run_settings` → `run_wifi`, and `run_wifi` → its own submenus).
Pre-existing, not introduced here, but it lives in this flow.

## 5. Against the three goals — what is and isn't substantiated

- **Loading time — NOT improved.** Measured, null result (§3). The radio wasn't in the way.
- **Battery — improved, by construction, not measured.** On the P4 the C6 is never brought up at boot and is
  unlinked when idle; that is a whole companion chip not running WiFi firmware. No bench power meter here, so
  the magnitude is unquantified — stated as structural, not measured.
- **Cores for the game — improved structurally, frame time not measured.** esp-hosted's priority-23 tasks and the
  wifi driver task simply don't exist unless something acquired the radio, and a cart launch forces a teardown.
  The launcher-side null result above suggests the idle radio's CPU cost was small, so the honest expectation is
  a small win, not a large one. A gameplay frame-time comparison was not run.

## 6. Commands run (reproduce)

```sh
# P4 (serial input so the menus can be driven over the console)
make flash APP=pico-e32-fake08 BOARD=guition-jc4880p443c PORT=/dev/ttyACM0 \
     DEFS="-D LAUNCHER=1 -D INPUT_BACKEND=serial"
# S3
make flash APP=pico-e32-fake08 BOARD=makerfabs-ili9488-r1 PORT=/dev/ttyUSB0 \
     DEFS="-D LAUNCHER=1 -D INPUT_BACKEND=serial"
```

Cycle test: boot, `d` `o` `d` to reach Settings → WIFI, `o` to open (acquire), `x` to leave (release), then `o`
again to re-acquire. Cart launch verified separately: Games → folder → `r` past the parent entry → `o`, which
logged `launch /sdcard/[Action-Adventure]/13 Jumps.p8.png` and started the VM.

## 7. Board state

Both boards are on `-D LAUNCHER=1 -D INPUT_BACKEND=serial` builds from this work (used for the menu-driving
tests). **Reflash the shipped touch build (`-D LAUNCHER=1`) before calling either board known-good for play.**

## 8. State & next

- `WC-5` implemented and verified on both boards.
- Follow-ups unchanged: `WC-2` lowercase glyphs, `WC-4` NTP / OTA / downloads — the latter being the first real
  consumers of `acquire`/`release`, which will also be the first test of two simultaneous reference holders.
