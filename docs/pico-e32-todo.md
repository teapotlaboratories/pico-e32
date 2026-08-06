# pico-e32 — master TODO index

The authoritative backlog root. This file only **points**; the detail lives in the linked
docs (per [`.ai/AGENTS.md`](../.ai/AGENTS.md) → *Plan first*).

- **Plan of record:** [`pico-e32-development-plan.md`](pico-e32-development-plan.md)
- **★ PRIMARY GOAL — port fake-08:** [`runtime/pico-e32-fake08-port.md`](runtime/pico-e32-fake08-port.md) — the runtime is a **port of fake-08** (MIT), not hand-written; replace only its `Host` layer. **Draw-only milestone is unblocked (no parts).** See plan §5.
- **Evidence base:** [`reference/pico-e32-runtime-feasibility.md`](reference/pico-e32-runtime-feasibility.md), [`pico-e32-silicon-decision.md`](reference/pico-e32-silicon-decision.md)
- **Hardware reference:** [`reference/pico-e32-makerfabs-boards.md`](reference/pico-e32-makerfabs-boards.md)
- **Display path (ILI9488 + driver):** [`hardware/pico-e32-display.md`](hardware/pico-e32-display.md) — pin map/bus/orientation status + its backlog (`DP-1`…`DP-9`); **`DP-9` — touch orientation at `ROTATE_180=false` is unverified on hardware**
- **Bench camera (HIL verification):** [`hardware/pico-e32-bench-camera.md`](hardware/pico-e32-bench-camera.md) — rig setup + its backlog (`BC-1`…`BC-6`); **`BC-1` done — the rig works and caught the Y-flip**
- **Guition JC4880P443C (ESP32-P4) board:** [`hardware/pico-e32-guition-jc4880p443c-p4.md`](hardware/pico-e32-guition-jc4880p443c-p4.md) — a SECOND, **RISC-V** target alongside the S3 boards; portable across S3+P4 (`GP-1`…`GP-8`). **`GP-1`–`GP-7` ALL DONE — merged to `main` via pico-e32#23 (2026-07-31).** **`GP-8` — SD cart launcher + native cover-art carousel — ✅ DONE on hardware (2026-08-03, both boards)** (P4 SDMMC needs the on-chip **LDO VO4** power; panel-agnostic via a new `board_carousel_layout()` seam; see the [worklog](worklog/2026-08-03-p4-sd-launcher-carousel.md)). **Follow-on (2026-08-05): main menu (Games/Settings/About) + fully board-driven layout (`body_scale` / `info_scale`) + working framebuffer-over-serial screenshots on both boards — the 08-03 `FB_DUMP` "dead end" is fixed (console CRLF + Task-Watchdog backtrace injection); see the [worklog](worklog/2026-08-05-launcher-menu-fbdump.md).** Real Celeste runs and is playable **with sound** on the P4: fake-08 on RISC-V, 480×800 ST7701S over MIPI-DSI, GT911 touch (`GP-5` ✅ HITL-verified), board-owned touch deck, and **`GP-6` ES8311 audio** (amp-enable GPIO11, synth on core 1; see [audio memory / doc](hardware/pico-e32-guition-jc4880p443c-p4.md)). Board boots, 32 MB PSRAM + early v1.3 silicon; needed a [RISC-V z8lua fix](runtime/pico-e32-fake08-port.md). **Gate #4 met on the P4** (playable ≥30 fps + sound + touch; loop 60 Hz, ~9.6 ms/frame headroom, 0 dropped frames — measured 2026-07-31). Apps: `firmware/pico-e32-p4-hello` (bring-up), `pico-e32-fake08` (Celeste), `pico-e32-p4-audio` (ES8311 tone test).
- **z8lua speedup research:** [`reference/z8lua-speedup-research.md`](reference/z8lua-speedup-research.md) — lever ranking; **profile before optimizing**
- **Bring-up log:** [`worklog/`](worklog/)
- **Firmware:** [`../firmware/`](../firmware/)

## Now — Phase 0 (de-risk on the 3.5" ILI9488 board)

> **Phase 0 is complete.** Its de-risking firmware apps (`pico-e32-luabench` / `-display-test` / `-host`) were
> **removed** as superseded by the fake-08 port — the gate results below stand, with detail in the linked worklogs.

| # | Item | Gate | Where | Status |
|---|------|------|-------|--------|
| B | z8lua interpreter throughput on the LX7 | **#2** ≤ 33 ms/frame of work (30 fps) | `pico-e32-luabench` *(removed)* | ✅ **measured on hardware — passable** (~1.6 M VM inst/s, ~2.5× under target, in the pass window). **Needs `-fjump-tables`** (~3×). See [worklog](worklog/2026-07-14-phase0-gate2-luabench.md) |
| A | ILI9488 scaled blit + FPS | **#1** ≥ 30 fps @ 256² | `pico-e32-display-test` *(removed)* | ✅ **GATE #1 PASSES — measured honestly, and the image is correct.** **blit-only 393.0 fps**, **end-to-end 210.6 fps** (expand+scale+blit every frame) vs a **610.4 fps** bus ceiling — 7–13× the gate, frame time 2.54/4.75 ms with ~zero spread. ✅ **Y-flip FIXED** — the glass is mounted mirrored; `.mirror_y = true` → `offset_rotation = 4`; L-pattern verified **upright** by camera. Panel confirmed **live during the timed window** (tearing visible while the palette animates), so this is not another 288. Driver **LovyanGFX** on rev-1 pins (WR=35/DC=36/CS=37). See [worklog](worklog/2026-07-16-yflip-and-gate1-fps.md), [display doc](hardware/pico-e32-display.md) |
| C | Trivial cart end-to-end (minimal `ESP32Host`) | **#3** cart ≥ 30 fps on panel | `pico-e32-host` *(removed)* | ⚠️ **groundwork done — 161.5 fps** (z8lua + ili9488 + trivial cart). **Panel image now VERIFIED** — the L-pattern cart renders **upright and correct** on the glass (2026-07-16), which was the last thing blocking this; `BC-1`/`BC-3` are done. Two caveats before calling the gate: the 161.5 fps predates the Y-flip fix **and** carries a task-watchdog backtrace inside its 1-second reporting windows (the loop never yields — same bug fixed in Track A), so **re-measure it**. And the framebuffer checksum covers less than it claims — see [`DP-4`](hardware/pico-e32-display.md#open-items). See [worklog](worklog/2026-07-14-phase0-gate3-host.md), [rig](worklog/2026-07-15-bench-camera.md) |
| B+ | **Gate #2 real-cart confirmation** — Celeste on the S3 | (confirmation + optimization scoping) | `pico-e32-luabench` *(removed)* | ✅ **real Celeste = ~15.8 ms/frame avg** (input-insensitive; per-room 5–40 ms, object-count-driven) → **solid 30 fps** (dense rooms near budget), 60 fps room-dependent; drawing runs on core 1. ⚠️ **Scoped to levels 3–15** — levels 16–30 live in the map's shared-memory region, which wasn't extracted, so *half the game is unbenchmarked*; now unblocked by [`HG-1`](runtime/pico-e32-host-graphics.md). Not a bug (the bench says so), but the average may move. Optimization levers now **deferred** (only needed for 60 fps / heavy carts): globals→locals measured only ~14% on Xtensa; see [research](reference/z8lua-speedup-research.md) + [worklog](worklog/2026-07-14-phase0-gate2-luabench.md) |

## Next — Phase 1 (playable, on the ILI9488)

- **Graphics surface — [`runtime/pico-e32-host-graphics.md`](runtime/pico-e32-host-graphics.md)** (`HG-1`…`HG-7`):
  `spr`/`map`/`print` are still **no-op stubs**, so Celeste's logic runs at frame rate but has never drawn
  a pixel. **Not parts-blocked, and verifiable without the camera** (host frame dump → PNG) — the one
  Phase-1 item that can move right now. `HG-1` ✅ (sprite sheet extracted), `HG-5` ✅ unblocked (font is CC-0 from Lexaloffle). See [worklog](worklog/2026-07-15-host-graphics.md).
- **Port fake-08** → `ESP32Host` — the primary runtime goal. **Real Celeste now plays end-to-end** on the
  panel (2026-07-18): draw-only port ✅, SD cart loader ✅, input seam ✅ (**serial + touch/FT6236 both
  HITL-verified** driving Celeste — IN-2 done), and the **fps fixed** (the host resumes fake-08's loop at
  60 Hz — a 30 Hz resume ran 30 fps carts at half speed; `CONFIG_FREERTOS_HZ=1000` for smooth pacing; opt-in
  on-screen FPS HUD). A **hands-free play-test now clears two full Celeste levels** ("100 M" → "200 M" →
  "300 M") over serial and self-verifies over the wire — each room's input is solved offline against a physics
  twin ([`test/playtest/celeste/celeste_solver`](../test/playtest/celeste/celeste_solver)) and delivered frame-synced to a new
  position-telemetry stream (`TELEMETRY=1` + `INPUT_HOLD_FRAMES=1`); run
  [`test/playtest/celeste/celeste_playtest.py`](../test/playtest/celeste/celeste_playtest.py).
  See the [input backlog](runtime/pico-e32-fake08-input.md) and the worklogs
  [fps-resume](worklog/2026-07-18-fake08-celeste-fps-resume.md) +
  [play-test clear](worklog/2026-07-18-celeste-playtest-clear.md). **The only seam still blocking
  Gate #4 is audio** (MAX98357A, parts-blocked); the physical-button **I²C expander** is also parts-blocked
  (touch needs none). The hand-written `HG-*` draw API is a de-risking harness, **superseded** by fake-08's
  own graphics. Plan in [`runtime/pico-e32-fake08-port.md`](runtime/pico-e32-fake08-port.md).
- **Gate #4:** a real cart playable ≥ 30 fps with sound + input; set the 30-vs-60 fps policy. **✅ MET on the ESP32-P4 (2026-07-31)** — Celeste playable with ES8311 sound + GT911 touch, loop 60 Hz / ~9.6 ms-per-frame headroom / 0 dropped frames (audio synth on core 1). Policy: host resumes at 60 Hz, carts self-divide to their native 30/60 fps; the P4 has headroom for 60 fps carts. **On the S3 it stays audio-parts-blocked** (no onboard codec; needs MAX98357A) — the P4's onboard ES8311 is what unblocked it.
- Parts to buy: MAX98357A + speaker (audio); optionally an I²C GPIO expander + buttons for physical input
  (touch via the on-board FT6236 needs none). microSD + slot are on-board. (See plan §7.)
- **Play-test harness → agent-solved carts + on-device FPS — [`test/playtest/README.md`](../test/playtest/README.md)**
  (`M0`…`M9`): the Celeste play-test restructured into a reusable rig whose goal is **measuring real on-device
  FPS across full playthroughs of arbitrary carts**, with the playthrough produced by an **agentic AI (no
  human)** driving a deterministic **gym** on the exact device VM. A solution is a replay-able `Trace` that
  must clear on **both** the sim and the device (proven for Celeste). `M0`–`M3` done (reorg; replay-from-root;
  `Trace` + dual-replay + host↔device frame-count sync; beam search demoted to an optional tool); next `M4`
  (agent-facing gym) → `M5` (spawned Celeste solver agent, isolated per-cart). eris VM savestates diagnosed +
  **parked** (multi-bug, matches upstream `47c48ad`). See [worklog](worklog/2026-07-19-playtest-harness-agent-solve.md).

## Later — Phase 2+ (the 4.0" ST7701 board)

- Port the host to the ST7701 **RGB** path; run **Gate #5** (RGB drift soak test) before trusting it.
  **Driver already in hand:** LovyanGFX (already vendored) ships `Bus_RGB` + `Panel_ST7701` and a
  ready-made config for this exact Makerfabs 4" board — same library as the 3.5" i80 driver. Detail +
  the Gate-5 caveat (it's a PSRAM-framebuffer panel by design) in [plan §2b](pico-e32-development-plan.md#2b-verified-hardware--makerfabs-40-st7701-480480-ordered).
- Enclosure + (only if custom) a PCB with the display that survives Gate #5.

## WiFi connectivity (`WC-*`)

- **`WC-1` — S3 WiFi foundation — ✅ DONE, hardware-verified; MERGED to main 2026-08-05 (pico-e32 `#30`).** New
  `components/wifi` (`esp_wifi` STA: scan / connect / status / NVS-persist / boot auto-connect). Backend chosen by
  IDF target in the component CMake: native `esp_wifi` on the S3, `esp_wifi_remote` → esp-hosted → C6 on the P4 (see
  `WC-3`). Settings → **WIFI** submenu: status, scan the air, deck-driven **on-screen keyboard** for the password,
  connect + persist. `BOARD_HAS_WIFI` gates the menu. OTA-ready 16 MB partition table added. Verified on the S3:
  scan → keyboard → connect → persist → boot auto-reconnect (joined `Tukang Ketoprak`, IP 192.168.7.228).
- **`WC-2` — lowercase font glyphs.** The PICO-8 font (`pico8_font.h`) is **uppercase-only** (`glyph_rows` upper-cases
  input), so a typed lowercase WiFi password *displays* as uppercase — it is **stored** with correct case, so the
  join works, but the user can't visually distinguish case. Add lowercase glyphs (a…z) so the password field (and any
  future mixed-case text) reads true. Low priority, isolated to the font table + `glyph_rows`.
- **`WC-3` — P4 WiFi via the ESP32-C6 companion (project Gate 4) — ✅ DONE, hardware-verified;
  MERGED to main 2026-08-05 (pico-e32 `#30`).** The P4 has no native radio; the on-board C6 comes up over SDIO/esp-hosted +
  `esp_wifi_remote`, `BOARD_HAS_WIFI` is defined for the P4, and the same `components/wifi` front-end drives it.
  Coexistence solved (WiFi + MIPI-DSI + SD all live in one boot). Two P4-specific traps and their fixes:
  - **esp-hosted's boot auto-init hangs.** esp-hosted inits itself from a C global constructor
    (`ESP_ERROR_CHECK(esp_hosted_init())`) before `app_main`; in this firmware that stalls the P4 in the C6 SDIO
    bring-up and `app_main` is never reached. Fix: defer the init to `app_main` (`wifi_mgr_init` does
    `esp_hosted_init()` + `esp_hosted_connect_to_slave()`), and neutralize the constructor **at build time** from the
    project CMake (esp-hosted stays fetched byte-identical to upstream; the transform re-applies after any clean).
  - **SD vs C6 shared SDMMC host.** The P4 has a single SDMMC host; the microSD (slot 0) and the C6 SDIO link (slot 1)
    can't both init it. Fix: drive the SD over **SPI** instead (`BOARD_HAS_SD` — same seam as the S3), which frees the
    SDMMC host entirely for the C6. `board_sd_config()` powers the card rail (on-chip LDO VO4) + fills the SPI wiring.
  - Verified: one boot mounts the SD over SPI **and** identifies the C6 (`Identified slave [esp32c6]`, STA up) with the
    launcher rendering. Driver-level connect proven earlier (joined `Tukang Ketoprak`, IP 192.168.7.212).
- **`WC-4` — build on the foundation:** NTP clock (About/real-time), OTA firmware update (partition table already
  OTA-ready), and network cart downloads (non-Splore). Each layers on `WC-1`. These are the **consumers** of the
  on-demand model in `WC-5` — each one acquires the radio, does its transfer, and releases it.
- **`WC-5` — WiFi off by default; on-demand only — ✅ DONE, hardware-verified on both boards (2026-08-06).** *Why:* the radio currently comes up on every boot and stays up
  for the whole session, which costs boot/loading time, battery, and CPU that should belong to the game — on the P4
  esp-hosted's tasks run at **priority 23** and the C6 stays powered even when nothing is using the network. A
  handheld that is mostly playing offline carts should have its radio off almost always.
  - **Model — pure on-demand, no boot connect and no persisted "on" toggle.** The radio is brought up only by a
    caller that needs it and dropped as soon as that caller is done. Refcounted `wifi_mgr_acquire()` /
    `wifi_mgr_release()` so overlapping users (e.g. an OTA check while the WiFi screen is open) compose correctly.
    Settings → **WIFI** holds a reference while the screen is open, so scan/join still work exactly as now; saved
    credentials still persist and are used by `wifi_mgr_autoconnect()` when a consumer acquires.
  - **Teardown is full, on BOTH boards:** `esp_wifi_stop` + `esp_wifi_deinit`, unregister the event handlers and
    destroy the STA netif; on the P4 additionally drop the esp-hosted link (`esp_hosted_deinit`) so the C6 stops
    drawing power and its priority-23 tasks go away. Investigate holding the C6 in reset (GPIO54) while idle so it
    isn't merely unlinked but actually off.
  - **Games get the whole machine:** launching a cart forces a teardown regardless of refcount — a cart never needs
    the network, and this is the point of the change.
  - **Verified** ([worklog](worklog/2026-08-06-wifi-on-demand.md)): no WiFi in either boot log until something
    acquires; teardown returns **126.5 KB** (P4) / **38.0 KB** (S3) of heap, and a second acquire lands within
    **48 bytes** of the first, so the cycle is repeatable and doesn't leak — `esp_hosted_deinit()` *does* allow a
    clean re-init, which was the main risk. Cart launch confirmed still working with the forced teardown in path.
  - **Null result, recorded honestly:** this did **not** improve loading time. P4 cover-art load is **64.0 ms
    median with the radio off — identical to with it on**; that path is SPI+decode bound and the idle radio wasn't
    contending. Battery is improved by construction (the C6 is never brought up) but **unmeasured** — no bench
    power meter. Gameplay frame time was **not** measured; the CPU win is structural (no priority-23 tasks
    resident) and, given the null result above, expected to be small.

- **`WC-6` — put the P4's SD back on SDMMC now that WiFi is on-demand — ✅ **STEP 1 DONE, hardware-verified
  (2026-08-06)**; step 2 deferred until a consumer needs it. *Why:* measured
  2026-08-06 by splitting the cover load into read vs decode — **SDMMC 4-bit reads at 10.20 MB/s (3.6 ms per
  ~38 KB cover) against SPI's 1.43 MB/s (26.3 ms): 7.1× faster**, taking total cover load from **55.4 ms to
  32.5 ms (−41%)**. Decode is ~29 ms either way and is the floor. This is the real lever on P4 loading time — the
  `WC-5` on-demand work was measured and did *not* move it.
  - **Why it's newly possible:** the SD went to SPI only because the C6 needed the single SDMMC host and WiFi was
    then always-on. Since `WC-5` the radio is off unless acquired, so the host is free almost all the time.
  - **The conflict that remains:** network cart downloads (`WC-4`) want SD *and* WiFi at once. **Resolution (owner):
    swap the SD onto SPI for the duration of a network session.** The SD and the C6 then run simultaneously exactly
    as they do today — that pairing is already shipped and proven — but it becomes the *temporary* mode instead of
    the permanent one. Better than buffering the download in PSRAM: no size ceiling, and a download can stream
    straight to the card. The SD is slow (1.43 MB/s) only while the radio is up, which is irrelevant when a cart is
    ~38 KB and the network is the bottleneck.
  - **Shape:** the board exposes **both** SD drivers on the same TF pins — SDMMC 4-bit (fast, default) and SPI
    (slow, network mode) — plus a switch. A network session is: close all file handles → unmount SDMMC + release
    the host → mount SD over SPI → `wifi_mgr_acquire()` → transfer → `wifi_mgr_release()` → unmount SPI → remount
    SDMMC. Sequenced by the **app**, not the wifi component: the wifi manager must not learn about storage, and the
    board owns both drivers. Rule: no file handle may be held across a mode switch.
  - **GATE — ✅ PASSED on hardware (2026-08-06).** The SDMMC host *can* be handed over at runtime. Probed the full
    round trip on the P4: SD mounted 4-bit → `esp_vfs_fat_sdcard_unmount()` → `Identified slave [esp32c6]` +
    `radio up` → `radio down` → SD **remounted** over SDMMC (44 ms). The original finding was only that the two
    cannot be initialised *simultaneously*; sequential handover is fine.
  - **Trap found while probing (do not repeat):** `esp_vfs_fat_sdcard_unmount()` **already** deinitialises the host
    and frees the card — IDF `components/fatfs/vfs/vfs_fat_sdmmc.c`, `unmount_card_core()` calls
    `call_host_deinit(&card->host)` then `free(card)`. Calling `sdmmc_host_deinit()` yourself afterwards is a
    double-deinit and **panics the board into a boot loop** (`Guru Meditation ... MCAUSE 0x1f` right after the
    unmount log). Unmount, and nothing else. Leave the LDO VO4 rail powered — SPI mode needs it too, so it should
    survive the switch rather than being torn down and re-acquired.
  - **Step 1 — ✅ DONE ([worklog](worklog/2026-08-06-p4-sd-sdmmc-handover.md)).** SD on SDMMC by default; the WiFi
    screen unmounts it (releasing the host) for the duration and remounts on the way out, including on the
    acquire-failure path. Measured in the shipped build: **cover load 64.0 → 39.4 ms median, −38%** (read 26.3 →
    3.6 ms, 7.1×; decode ~29 ms unchanged and now ~75% of what's left, so the bus is no longer the bottleneck).
    Remount takes 44 ms and the card is fully usable after (19 folders / 402 entries re-read). Sequencing lives in
    the launcher — `wifi_manager` still knows nothing about storage. S3 untouched.
  - **Step 2 — deferred until needed.** SD on SPI *simultaneously* with the radio, for a download that wants
    storage and network at once. Already proven as a pairing (it is exactly what shipped before step 1); needs the
    board to carry both drivers plus a mode-switch seam and a no-open-file-handles rule across the switch. Blocked
    on nothing except `WC-4` producing a consumer.
  - **Verify:** the gate above; cover-load split before/after (expect ~32.5 ms); a full
    SDMMC → SPI → WiFi → SPI → SDMMC cycle repeated several times without losing the card; and SD writes working
    while the radio is up.
  - **S3 is unaffected** — native radio, no shared host; it keeps SPI.

## Open decisions

- Bytecode-precompile strategy (build-time vs load-time) — plan §10.
- Whether the 4" board clears Gate #5 for heavy PSRAM-heap carts — hardware-only answer.
