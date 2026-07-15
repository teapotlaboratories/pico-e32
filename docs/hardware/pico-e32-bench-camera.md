# Bench camera (M5Stack Timer Camera F) — hardware-in-the-loop capture

The bench camera is equipment, not handheld firmware: it is aimed at the panel under test
and serves a still JPEG over HTTP so display changes can be verified by *looking at what
actually renders*, per [`.ai/AGENTS.md` → Verifying changes](../../.ai/AGENTS.md#verifying-changes).

- **Firmware:** [`firmware/pico-e32-bench-cam`](../../firmware/pico-e32-bench-cam) (target `esp32`, board `m5stack-timer-cam`)
- **Capture tool:** [`tools/capture_frame.sh`](../../tools/capture_frame.sh)
- **Frames land in:** `captures/<timestamp>[-label].jpg` (gitignored — see *Evidence* below)

## Boards on the bench

| role | board | chip |
|------|-------|------|
| **device under test** | Makerfabs ILI9488 (ESP32-S3 N16R2) | ESP32-S3 |
| **bench camera** | M5Stack Timer Camera F (OV3660, fisheye) | ESP32-D0WDQ6-V3 |

> Both boards enumerate as `/dev/ttyUSB*` and **the numbering is not stable** — it depends on
> plug order. Confirm which is which before flashing, so you don't flash the camera firmware
> onto the board under test:
> ```sh
> esptool.py -p /dev/ttyUSB0 flash_id | grep 'Chip is'
> # ESP32-D0WDQ6-V3 -> the bench camera
> # ESP32-S3        -> the board under test
> ```

## One-time setup

**WiFi credentials are never stored in the tree** — pass them on the build command, where they
reach the firmware as compile-time macros:

```sh
make flash APP=pico-e32-bench-cam BOARD=m5stack-timer-cam PORT=/dev/ttyUSB0 \
     WIFI_SSID='my ssid' WIFI_PASS='my pass'
```
The build fails with an `#error` if they are missing.
On boot it logs its address over UART:
```
I (…) camera: Detected OV3660 camera
I (…) bench-cam: ==== bench-cam ready:  http://192.168.x.y/capture  ====
```

Then point the capture tool at it:
```sh
cp tools/bench_cam.env.example tools/bench_cam.env   # set BENCH_CAM_HOST=<ip>
```

### Gotchas that cost real time

- **Baud:** this board's FTDI bridge cannot sustain idf.py's default 460800 — the chip syncs
  at 115200 and then dies on the baud switch with `No serial data received`. Pinned to 115200
  in [`boards/m5stack-timer-cam/board.mk`](../../boards/m5stack-timer-cam/board.mk); no flag needed.
- **Port permissions:** the FTDI node is re-created `root:dialout` on *every board reset*, so a
  one-off `chmod` does not survive. `/etc/udev/rules.d/99-esp-serial.rules` grants `MODE="0666"`
  to the bridges used here (FTDI/CP210x/CH340/Espressif native USB).
- **Lens film:** these ship with a protective film over the lens — captures come back black,
  and auto-exposure will *not* save you. Peel it before blaming the firmware.
- **A failed camera probe self-diagnoses:** if `esp_camera_init()` fails, the firmware scans the
  SCCB bus, tests the lines for pull-ups, sweeps every plausible pin pair, and tries each known
  board pin map — the log distinguishes a pin-map error from dead/unpowered hardware.

## Capturing

```sh
tools/capture_frame.sh                 # -> captures/20260715-101500.jpg
tools/capture_frame.sh gate1-bars      # -> captures/20260715-101500-gate1-bars.jpg
```
It prints the path it wrote. The endpoint drops one frame before returning, so you always
get a *current* image rather than a stale buffered one.

## The loop (display changes)

```sh
make flash APP=pico-e32-display-test BOARD=makerfabs-ili9488 PORT=/dev/ttyUSB1
sleep 2 && tools/capture_frame.sh gate1-bars
# inspect the frame; adjust madctl / swap_color_bytes; repeat
```
**Judge pass/fail against the captured frame** — not against "the draw call returned".

## Framing & camera realities

- **Keep the framing fixed** once set (tape the camera down) so captures are comparable
  across runs; a moved camera invalidates before/after comparisons.
- Account for **backlight glare, focus, and colour cast** — the panel is emissive, so the
  camera's white balance can shift hues. Judge *relative* colour (bar order, which bar is
  red vs blue) rather than absolute RGB values.
- Camera is SVGA (800×600) JPEG — ample to read a 320×480 panel. The F is a **fisheye**: expect
  barrel distortion at the edges, so centre the panel in frame.

## Evidence

`captures/` is gitignored (binary churn). When a frame is *evidence* for a worklog, copy it
to `docs/hardware/evidence/` and reference it from the worklog, or paste the observation
plus the capture filename.

## Open items

- **Gate #1** (ILI9488 palette bars) and **Gate #3** (trivial cart image) are throughput- and
  framebuffer-verified but **not visually confirmed** — they are the reason this rig exists.
