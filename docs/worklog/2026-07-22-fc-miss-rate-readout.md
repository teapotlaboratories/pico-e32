# 2026-07-22 — fc-scheduled deadline miss-rate readout (the last quantitative number)

Goal: surface the on-device fc-scheduled **deadline miss-rate** — the one number the sim cannot give, listed as
the open acceptance item for IN-5 and in the previous worklog's Open/next. The clears (rooms 100→400 M on the
board) already *prove* misses are ~0 at lead k=2, but the count never left the board: `input_sched_stats()`
existed in `input_scheduled.c` but was **declared nowhere** and **called nowhere**.

Branch `fc-miss-rate-telemetry` (off `main` after PR #14 landed). Feature work — its own branch/PR when the
owner asks to land it.

## What the readout is

`input_sched_stats(fed, miss, applied)` on the scheduled backend already accumulates, per its comments:
- **fed** = valid fc-commands the board saw (incl. ring-drops, which never made it into the table),
- **miss** = commands that arrived *after* their target frame — a deadline miss — plus ring-drops + table
  overflow (every command that failed to apply on time),
- **applied** = commands that reached their active window.

`miss/fed` is the deadline miss-rate the whole fc-scheduled design turns on. This change makes it leave the
board and get reported by the host drivers.

## Firmware — the seam + the emit

- **Seam (`input.h`):** declared `input_sched_stats(uint32_t*, uint32_t*, uint32_t*)` (it was a definition with
  no declaration). Following the seam's own convention (each backend no-op's `input_set_frame`), the other four
  backends (stub / serial / touch / i2c) get a **no-op stub that writes zeros**, so `main.cpp` can call it
  unconditionally regardless of which backend is compiled.
- **Emit (`main.cpp`, TELEMETRY loop):** a periodic ASCII line `TS <fed> <miss> <applied>`, streamed ~1×/s
  (every 60 Steps = ~30 game-frames at 2 Steps/frame), **gated to the scheduled backend** (checked once via
  `strcmp(input_backend_name(), "scheduled")` before the loop). Placed *after* the per-frame telemetry emit and
  *outside* the `step_us`/`draw_us` timing window, so it perturbs neither the fps numbers nor any other build.
  - **Distinct prefix `TS`** (not `T`) so the host's telemetry parser skips it; and an ASCII line never
    contains `0xAA`, so the binary telemetry reader's sync-scan skips it too — safe to interleave on either
    stream. Non-scheduled telemetry builds (e.g. the M8 serial play-test) emit nothing new and are unchanged.
- **`applied` count fix (`input_scheduled.c`):** the first on-board run reported `applied=180 > fed=111` —
  nonsensical. Root cause: `input_poll` runs **more than once per frame-clock value** (the fake-08 tick polls
  input ~2× per emitted `fc` on a 30 fps cart), and `s_applied` was `++`'d at every `fc==G` poll, so a hold=1
  command counted twice. `fed`/`miss` are counted once per ring-drain, so the **miss-rate was already
  correct** — only the redundant `applied` field doubled. Fixed with a per-command `counted` flag: tally each
  command exactly once, at its first active frame (robust to poll frequency). Host-test unchanged (`applied=7`
  — it polls once/frame, so old and new agree there); on-board `applied` now equals `fed`.

## Host — parse + report (cart-agnostic)

- **`fc_sched.py`** (the single source of the wire protocol) gains `parse_stats(line) -> {fed, miss, applied,
  miss_rate}` (or `None`), symmetric with `encode_cmd`. It's the reverse-direction half of the protocol, so it
  lives with it.
- **The three device drivers** capture the latest `TS` line in their read loops and report it:
  `celeste/fc_device.py` `drive_device` (single room) + `drive_device_chain` (open-loop), and
  `live.py` `drive_device_predictive` (twin-in-the-loop, which the racer uses too). Each prints
  `fc-scheduled miss-rate: <miss>/<fed> cmds missed the deadline (X.X%), <applied> applied on time [lead k=N]`
  and returns `sched_stats` in its result dict. The periodic readout lags the final tally by ≤1 s; immaterial
  when misses are ~0 (documented).

## Verified (no hardware needed for these)

- **Host protocol tests 9/9** (`test/playtest/test_fc_sched.py`, +2 new): `parse_stats` decodes `TS` (incl. a
  trailing `\r`, and `0/0/0` without divide-by-zero) and **rejects** a `T` telemetry frame, an ESP log line,
  and malformed input — so it never misfires on the stream it shares with telemetry.
- **Input backend gcc host-test** (`components/input/host_test/run.sh`): `input_scheduled.c` still compiles
  clean against the real `input.h` and its ring/parser/apply-by-`fc`/miss logic still matches the Python twin
  (`fed=8 miss=1 applied=7`).
- **Full ESP32-S3 firmware build — exit 0** (`make build … DEFS='-D INPUT_BACKEND=scheduled -D TELEMETRY=1
  -D SHOW_FPS=1 -D CENTER_GAME=1'`): `main.cpp` compiled clean, binary generated (0xf2470 B, 5% partition
  free). The emit code is variant-independent (declared outside the BINARY/HOST_CFG/plain `#if`), so the one
  build covers all three telemetry variants; this build is the plain-ASCII path the fc-scheduled Celeste run
  uses.

Also fixed two things the recent backlog reconciliation flagged in the files I touched: the input CMakeLists
`FATAL_ERROR`/comment listed backends as `stub|serial|touch|i2c` and **omitted `scheduled`**; and the input
doc's IN-5 said the firmware was "not yet written" (it's merged + hardware-validated). Both corrected.

## Verified ON HARDWARE (owner green-lit the flash)

Flashed the validated build and ran the driver on the physical Makerfabs ESP32-S3 (`/dev/ttyUSB0`, the CP2104;
identified read-only via `/dev/serial/by-id/` — no esptool probe, so the bench camera's tuning was untouched
[[bench-rig-gotchas]]). The `TS` line streams and the drivers report it:

```
# room (0,0) open-loop:
open-loop 100 M: delivered 90/90 fc-commands  ->  CLEAR at fc=464
fc-scheduled miss-rate: 0/111 cmds missed the deadline (0.0%), 111 applied on time  [lead k=2]

# full chain 100->200->300->400 M open-loop:
delivered 579/579 fc-commands  ->  CLEAR at fc=1454
fc-scheduled miss-rate: 0/600 cmds missed the deadline (0.0%), 600 applied on time  [lead k=2]
fps over 723 game-frames: 7.6/28.5/30.0
```

**0 deadline misses across a 600-command, 4-room, 723-game-frame run at k=2** — the design's core prediction
(misses ~0 at k=2), now measured on the board, the number the sim cannot give. `fed` = 579/90 room commands +
~21 title-skip warmup jumps; `applied == fed` after the count fix; `fc=1454` is bit-identical to the recorded
400 M clear (deterministic). Reproduce:

```
make flash APP=pico-e32-fake08 BOARD=makerfabs-ili9488-r1 PORT=<CP2104> \
  DEFS='-D CELESTE=1 -D FORCE_FLASH_CART=1 -D INPUT_BACKEND=scheduled -D TELEMETRY=1 -D SHOW_FPS=1 -D CENTER_GAME=1'
python3 test/playtest/celeste/fc_device.py <CP2104> --openloop --room0   # or --to400 for the full chain
```

## Open / next

- **Sweep the reported miss-rate vs lead k** (1/2/3) and host `low_latency` on/off to confirm it *moves* as the
  sim's jitter model predicts — the quantitative half of IN-5's acceptance. At k=2 on this tuned path it reads a
  flat 0%; the interesting data is where it starts to climb (expect k=1 to show non-zero on a loose host). This
  is the natural next experiment now that the number is exposed on the board.
