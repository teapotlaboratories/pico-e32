# WiFi & networking (`WC-*`)

The authoritative backlog for everything radio-side on both boards — the connection UI, the power policy, and the
features that build on them (NTP, OTA, cart downloads). Indexed from
[`docs/pico-e32-todo.md`](../pico-e32-todo.md), which only points here.

**Where the code lives:** `components/wifi` (the STA front-end, one implementation over two backends),
the launcher's WIFI screen in `firmware/pico-e32-fake08/main/carousel_launcher.cpp`, and the per-board
`BOARD_HAS_WIFI` seam. Board-specific hardware notes (C6 wiring, the shared SDMMC host) live in
[`../hardware/pico-e32-guition-jc4880p443c-p4.md`](../hardware/pico-e32-guition-jc4880p443c-p4.md).

## Backlog

- **`WC-1` — S3 WiFi foundation — ✅ DONE, hardware-verified; MERGED to main 2026-08-05 (pico-e32 `#30`).** New
  `components/wifi` (`esp_wifi` STA: scan / connect / status / NVS-persist / boot auto-connect). Backend chosen by
  IDF target in the component CMake: native `esp_wifi` on the S3, `esp_wifi_remote` → esp-hosted → C6 on the P4 (see
  `WC-3`). Settings → **WIFI** submenu: status, scan the air, deck-driven **on-screen keyboard** for the password,
  connect + persist. `BOARD_HAS_WIFI` gates the menu. OTA-ready 16 MB partition table added. Verified on the S3:
  scan → keyboard → connect → persist → boot auto-reconnect (joined `Tukang Ketoprak`, IP 192.168.7.228).
- **`WC-2` — case-visible text — ✅ DONE, hardware-verified (2026-08-07). Resolved by COLOUR, not glyphs.**
  - **The problem, stated properly.** `glyph_rows()` folds `a-z` to `A-Z`
    (`carousel_launcher.cpp`: `if (c >= 'a' && c <= 'z') c -= 32;`), so **every mixed-case string in the UI
    renders uppercase** — not just the password. Confirmed on the panel: an SSID shows as
    `TUKANG KETOPRAK`, a version as `OTA-TEST-2`. The fold is *intentional and documented* in
    `assets/pico8_font.h` ("PICO-8 renders lowercase as caps"), so this is a deliberate behaviour change, not
    a bug fix — PICO-8 itself has no descender lowercase.
  - **Where it actually hurts:** the WiFi **password field**. You cannot see what you typed, so a mistyped
    password is indistinguishable from a wrong one. Everywhere else (SSID lists, cart and folder names,
    version strings) all-caps is legible and arguably on-style.
  - **The hard constraint:** glyphs are **3 wide × 5 tall** on a shared baseline (row 4), advancing 4 px and
    6 px per line. **A 5-row cell has no descender row**, so `g j p q y` cannot descend. Real lowercase in
    this cell means x-height forms (rows 2–4) plus ascenders (rows 0–1) for `b d f h k l t`, and the five
    descender letters sitting on the baseline — legible, but they will read as small-caps-ish. Growing the
    cell to 6 rows is the only way to get true descenders, and that changes the height of **every** line of
    text on every screen.
  - **Blast radius:** the table is shared with `components/fake08/fps_hud.cpp` (digits only, so additive
    entries are safe there). The font is a **hand-authored placeholder** carrying no third-party data and is
    not fake-08's, so editing it raises no 1-to-1 porting concern.
  - **What was actually built.** Neither of the glyph options. Drawing them settled it: in a 3×5 cell with no
    descender row, lowercase `a`, `g` and `q` are *the same three rows* — "Tukang" rendered as "Tukana". A font
    that cannot separate a from g is useless for the password field this exists for. Growing to 3×6 fixed that
    but cost a 20% height increase on every line, i.e. a re-layout of every screen on both boards.
    **The owner's idea replaced both:** keep the one set of (cap-shaped) letterforms as the default, and carry
    case in **colour** — a capital is drawn in a fixed highlight (cyan; not the accent, see §5 of the worklog).
    No new glyphs, no ambiguity, no cell-height change,
    no re-layout.
  - **The catch, found by putting it on the panel:** every UI label was an uppercase string literal, so the
    whole interface turned accent-blue. Fixed by rewriting **108 display literals to lowercase** — they render
    as the same cap shapes, unmarked — leaving the accent to mean "selected row" and "a real capital in data".
    The keyboard's `KB_LOW`/`KB_UPP` rows are *character data*, not labels, and were deliberately left alone;
    shifting to `KB_UPP` marks the keys, which doubles as a caps indicator (this briefly regressed — see the
    worklog §8 — because the inversion guard was written as "any non-default background").
  - `case_col()` applies only on the normal background: where a caller has inverted the row (a selected pill, a
    highlighted key) fg/bg are a contrasting pair and the highlight stands down, so capitals never land as
    cyan-on-accent.
  - **Verify:** `FB_DUMP` captures of (a) the password field mid-typing showing the actual characters, and
    (b) the WiFi list against a real mixed-case SSID; plus both boards building, since the font is shared.
  - **Original note:** The PICO-8 font (`pico8_font.h`) is **uppercase-only** (`glyph_rows` upper-cases
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
- **`WC-4a` — OTA firmware update over WiFi — ✅ DONE, hardware-verified on both boards; MERGED to main 2026-08-06 (pico-e32 `#33`).** Update the handheld from Settings
  instead of a USB cable.
  - **Why now:** the infrastructure already exists — the 16 MB table has `otadata` + two 4 MB app slots
    (`firmware/pico-e32-fake08/partitions.csv`, added in `WC-1`), and the radio is a clean acquire/release away.
    Highest day-to-day payoff of the `WC-4` set and self-contained.
  - **Shape:** Settings → **SYSTEM UPDATE**. Shows the running build; on request it acquires the radio, fetches a
    small JSON manifest over HTTPS, compares versions, and if newer streams the `.bin` into the *inactive* OTA slot
    via `esp_https_ota`, then sets the boot partition and reboots. Radio released on every exit path (the WiFi
    screen's scope-guard pattern); on the P4 the SD host is lent out for the duration exactly as the WIFI screen
    does — an OTA writes to flash, not the card, so it needs no storage.
  - **Manifest** (owner-hosted, versioned so the format can change):
    `{"version":"<git-describe>","build":"<iso8601>","url":"<https url>","sha256":"<hex>","size":<bytes>}`.
  - **Safety — this is firmware that replaces firmware; it must not brick the board:**
    - Write only to the **inactive** slot (`esp_ota_get_next_update_partition`), never the running one.
    - **Verify before switching:** size + SHA-256 against the manifest, plus IDF's own image validation.
    - Mark the new image **pending-verify** and require it to boot to the launcher before
      `esp_ota_mark_app_valid_cancel_rollback()`; enable `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` so a bad image
      rolls back to the previous slot automatically instead of bricking.
    - **Refuse to start** on a low battery once that is measurable (not yet — note it, don't fake it).
    - A failed or aborted download must leave the running slot untouched and simply report.
  - **Verify (acceptance):** (a) end-to-end update on **both** boards from a locally-hosted manifest + image, with
    the version string visibly changing after reboot; (b) a **deliberately corrupted** image is rejected and the
    board still boots the old firmware; (c) power-pull mid-download leaves the board bootable; (d) the radio and
    (P4) the SD host are both released whichever way the screen is left, including the error paths; (e) an OTA with
    no network reports cleanly rather than hanging.
  - **Out of scope here:** signature verification (needs a signing key + secure boot decision — file separately if
    wanted), and delta updates.
  - **Bench endpoint:** [`tools/ota_server.py`](../../tools/ota_server.py) serves built images plus a
    manifest for each and prints the `CONFIG_PICO_E32_OTA_MANIFEST_URL` line to paste. It reads `target`,
    `version`, `sha256` and `size` **out of the image** (chip id from the image header, version from the app
    descriptor) rather than taking them on trust, so a manifest cannot disagree with the binary it describes —
    which is the exact failure the device-side checks exist to catch. Bench only: plain HTTP, no signing.

- **`WC-4b` — NTP clock and network cart downloads.** (was the rest of `WC-4`)
  **Original note:** NTP clock (About/real-time), OTA firmware update (partition table already
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
  - **Verified** ([worklog](../worklog/2026-08-06-wifi-on-demand.md)): no WiFi in either boot log until something
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
  - **Step 1 — ✅ DONE ([worklog](../worklog/2026-08-06-p4-sd-sdmmc-handover.md)).** SD on SDMMC by default; the WiFi
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
