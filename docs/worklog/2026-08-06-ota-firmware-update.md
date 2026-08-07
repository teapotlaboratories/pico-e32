# 2026-08-06 — OTA firmware update over WiFi (`WC-4a`)

Goal: update the handheld from Settings instead of a USB cable. Spec + acceptance criteria in
[`WC-4a`](../firmware/pico-e32-wifi-networking.md).

> **Running log — appended as the work happens**, per `.ai/AGENTS.md`. Earlier worklogs in this series were
> written at the end, which is the thing that rule exists to prevent; this one starts before the code.

## Why this is worth care

This is firmware that replaces firmware. Every other feature so far could fail by not working; this one can fail
by leaving the board unbootable. The plan is therefore biased to safety: write only the inactive slot, verify
before switching, and let the bootloader roll back if the new image doesn't reach the launcher.

## Starting state (2026-08-06, before any code)

- Partition table already OTA-capable from `WC-1`: `nvs`, `otadata`, `ota_0` 4 MB, `ota_1` 4 MB, `storage`
  (`firmware/pico-e32-fake08/partitions.csv`). Nothing to re-partition — which matters, because re-partitioning
  would wipe the saved WiFi credentials in `nvs`.
- Radio is on-demand (`WC-5`): `wifi_mgr_acquire()` / `wifi_mgr_release()`, full teardown at the last release.
- On the P4 the SDMMC host is lent to the radio for a session (`WC-6`); an OTA writes to **flash**, not the card,
  so it needs no storage while running — the same lend/repay the WIFI screen uses is enough.
- Both boards on the shipped touch build, `main` at `d65dcae`.

## Conformance fixes done first (this session, before starting)

Audited against `.ai/AGENTS.md` at the owner's request and found three real breaks, all from today's own churn:

1. **The P4 hardware doc still documented the SD as SPI** — wrong since `WC-6` put it back on SDMMC hours
   earlier. Violates "keep the wiring/pin map in the docs, not just the code". Pin-map row rewritten (SDMMC 4-bit,
   the runtime handover, and the `sdmmc_host_deinit()` double-deinit trap), plus a `GP-10` row.
2. **Memory said the P4 drives the SD over SPI** — the exact "confidently wrong memory" failure the rules call the
   most expensive artifact in the project. Rewritten, including the flip-flop history so the next session doesn't
   re-derive it.
3. **The master TODO held ~99 lines of `WC-*` detail** when the rule says it "only points; the detail lives in the
   area doc". Moved to a new [`docs/firmware/pico-e32-wifi-networking.md`](../firmware/pico-e32-wifi-networking.md);
   the index now carries a status table only.

A fourth, process rather than artifact: **worklogs were being written at the end, not as the work happened.** This
file is the correction.

## Log

### 1. Plan (before code)

Design as specced in `WC-4a`. Order of work:

1. `components/ota` — a small front-end mirroring `wifi_manager`'s shape: `ota_check()` (fetch + parse manifest,
   compare version) and `ota_apply()` (stream to the inactive slot, verify, set boot, reboot), blocking calls
   suited to a modal screen.
2. Settings → **SYSTEM UPDATE** screen, reusing the WIFI screen's radio + SD-host scope-guard pattern.
3. Rollback config (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) + a `mark_app_valid` once the launcher is reached.
4. A local HTTPS host for the manifest + image, so the whole thing is testable on the bench without shipping a
   public endpoint.

Open question to settle while building: the manifest/image host for bench testing — a self-signed local server
needs its CA baked in, which is fine for testing but must not become the shipped default.

### 2. Library choice — and a correction

Started by writing `esp_https_ota` into the spec, then read what `esp_https_ota_finish()` actually does before
committing to it:

```c
err = esp_ota_end(handle->update_handle);
...
err = esp_ota_set_boot_partition(handle->partition.staging);
```

(`vendor/esp-idf/components/esp_https_ota/src/esp_https_ota.c`) — it finalises the image **and switches the boot
slot in the same call**. There is no point between the two to compare the image against the manifest hash. The
only way to keep `esp_https_ota` would be to switch and then revert on mismatch, which is a worse story for a
brick-safety requirement than never switching at all.

**Decision: `esp_http_client` + `esp_ota_ops` directly.** Hash while streaming, compare, and only then
`esp_ota_end()` + `esp_ota_set_boot_partition()`. It also avoids a second pass reading ~1.5 MB back out of flash
to hash it, and there is in-repo precedent — `firmware/pico-e32-p4-c6-ota/components/ota_https/ota_https.c` uses
the same pattern for the C6 slave.

Recorded because the spec named the other library first: the initial choice was reasonable and wrong, and the
reason it was wrong is only visible in the implementation, not the docs.

### 3. mbedtls 4.x — the legacy SHA API is gone

`#include "mbedtls/sha256.h"` fails to compile on this tree: **IDF v6.0.2 ships mbedtls 4.1.0**, which is the
TF-PSA-Crypto split. The legacy low-level hash API moved to `mbedtls/private/sha256.h` and is no longer public;
the public path is **PSA Crypto** (`psa/crypto.h`, at
`components/mbedtls/mbedtls/tf-psa-crypto/include/psa/crypto.h`).

So the hashing is `psa_hash_setup(PSA_ALG_SHA_256)` / `psa_hash_update` / `psa_hash_finish`. No
`psa_crypto_init()` call is needed — IDF runs it during system init
(`components/mbedtls/port/esp_psa_crypto_init.c`, `ESP_SYSTEM_INIT_FN`). Worth knowing for anything else in this
repo that reaches for mbedtls hashing later.

A `psa_hash_setup` failure is treated as fatal for the update rather than "skip the check": an image that cannot
be verified is not flashed.

### 4. State: component builds

`components/ota` (`ota_manager.{h,c}`) compiles and links into the P4 build. Config added to
`sdkconfig.defaults`: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` (the anti-brick net) and the mbedTLS certificate
bundle for TLS against public CAs. Deliberately **not** `BOOTLOADER_APP_ANTI_ROLLBACK` — it burns eFuses, is
irreversible, and would block flashing an older build on a DIY handheld; it also belongs with a secure-boot
decision that is out of scope.

Nothing is on hardware yet and no UI exists — next is the SYSTEM UPDATE screen, then a local manifest/image host
to test against, including the negative cases (corrupted image rejected, power-pull mid-download).

### 5. UI + the two quoting traps

Settings gains a **SYSTEM UPDATE** row and `run_update()`: current version → CHECK (acquires the radio, lends
the SD host out on the P4 via the same `SdHostLoan` guard the WiFi screen uses, autoconnects, fetches the
manifest) → shows *what* it would install (version, size, build) → INSTALL with a progress bar and X to cancel →
reboot. The guard was hoisted to file scope so both network screens share one definition of the invariant.

Two traps, same root cause, both cost a test cycle:

1. **`-D OTA_MANIFEST_URL='"http://…"'` silently arrived empty.** Quotes do not survive
   make → `idf.py` → CMake → compiler. Moved to Kconfig (`main/Kconfig.projbuild`,
   `CONFIG_PICO_E32_OTA_MANIFEST_URL`), which quotes correctly — and is better anyway: the endpoint a build will
   trust is now visible config rather than a magic command-line string. Empty by default, and the menu says
   "NO UPDATE URL IN BUILD" rather than pretending.
2. **`-D BENCH_WIFI_SEED=1` did nothing at all.** `DEFS` flags only become compile definitions if
   `main/CMakeLists.txt` explicitly forwards them (`target_compile_definitions`) — there is a per-flag list. An
   unknown `-D` sets a CMake variable and is otherwise ignored, with no warning.

Also: **changing `sdkconfig.defaults` does not take effect on an existing build** — IDF only applies defaults
when generating `sdkconfig` the first time. The generated `build/<app>/<board>/sdkconfig` has to be deleted
(which forces a full rebuild).

### 6. A real bug found by the test setup

Seeding credentials at boot to avoid driving the on-screen keyboard failed silently: `wifi_mgr_save()` returned
an error because **NVS had never been initialised**. `nvs_flash_init()` only ran inside the radio bring-up, so
any credential call made without the radio having come up first was writing into uninitialised NVS and dropping
the write.

Not just test scaffolding — `wifi_mgr_save`/`load`/`forget` are legitimately callable without the radio (a
settings screen reading saved state, provisioning, this). Added an idempotent `ensure_nvs()` used by all three
and by the bring-up path. After the fix the seed returned `ESP_OK` and autoconnect found the network.

The UX flaw it exposed is fixed too: "NO NETWORK" conflated *no saved credentials* with *the join failed*, which
send the user to different places. Now "NO SAVED WIFI - JOIN ONE FIRST" vs "CANT REACH NETWORK".

### 7. Hardware results — both paths

**Happy path, P4, end to end:**

```
App version: 30834c5-dirty                       <- running
wifi: autoconnect -> Tukang Ketoprak
sta ip: 192.168.7.212
ota: manifest: ota-test-2 (1661328 bytes)
ota: target slot 'ota_1' @0x420000 (4096 KB)
ota: verified 1661328 bytes, boot slot -> 'ota_1'
rst:0xc (SW_CPU_RESET)
App version: ota-test-2                          <- NEW FIRMWARE RUNNING
ota: new image confirmed good (ESP_OK) — rollback cancelled
```

1.66 MB downloaded, hashed and switched in **7.6 s** (~218 KB/s over the C6). The version visibly changed, and
the pending-verify → mark-valid handshake completed.

**Corrupted image — the test that matters.** Served a byte-flipped image while advertising the good hash:

```
ota: manifest: ota-test-3-corrupt (1661328 bytes)
ota: target slot 'ota_0' @0x20000 (4096 KB)
E ota: SHA-256 mismatch — boot slot untouched
E ota:   manifest 14557f0482fd934e8953efdc34aa8e0a0d4a495f8f9ddbe01dd5cab08ef3caf2
E ota:   received  4946ff8c19ac2b7cec9b31c27156df289e9cf59ec99b18d88f93565648524d6f
```

Rejected, both hashes logged, and the board rebooted straight back into `ota-test-2` with the SD mounting and the
carousel drawing. **The bad image was written to flash and then simply never booted** — which is the whole design.

### 8. Acceptance status against `WC-4a`

| criterion | status |
|---|---|
| (a) end-to-end update, version visibly changes | ✅ P4. **S3 not yet tested** |
| (b) corrupted image rejected, old firmware still boots | ✅ verified |
| (c) power-pull mid-download leaves the board bootable | ⬜ not yet run |
| (d) radio + SD host released on every exit path | ✅ observed on the failure paths exercised |
| (e) no-network case reports cleanly | ✅ (this is how the NVS bug surfaced) |

Also outstanding: the whole test ran over **HTTP** against a local server. TLS is wired (cert bundle, plus an
`OTA_INSECURE` escape hatch for self-signed) but **has not been exercised** — so "HTTPS works" is unverified.

### 9. Actually looking at the UI — one real bug

Everything above was verified from serial logs. The screens had never been *seen*, which is precisely what the
repo's "capture a frame and look at it" rule exists to prevent. Captured them via `FB_DUMP`:

- **Settings row — BUG.** `SYSTEM UPDATE` as a label plus a full git-describe version rendered as
  `SYSTEM UPDA79B369F-DIRTY`: the label and the right-aligned value **overlapped mid-row**. Invisible to every
  log-based check, obvious in one screenshot. Fixed by shortening both — the row is now `UPDATE / 79B369F` (the
  screen it opens is titled SYSTEM UPDATE, and the short hash is the part anyone reads), which also matches the
  visual rhythm of the ACCENT/WIFI/BRIGHTNESS rows.
- **Confirm screen — correct.** `CURRENT` dim, `NEW` in the accent colour, plus size and build stamp, so the
  install is approved against what it actually is rather than a bare "update available".
- **Update home — correct.** Title, `O SELECT  X BACK`, `CURRENT`, and the highlighted `CHECK FOR UPDATE` pill.

Also fixed a small gap found while capturing: the confirm loop had no `FB_DUMP` hook, so unlike every other
screen it could not be screenshotted. Added — a screen that can't be captured can't be reviewed.

Frames in the scratchpad (not committed, per the captured-frames rule).

### 10. Centring the action buttons

Owner asked for "CHECK FOR UPDATE" to sit in the middle. Rather than special-casing one screen, the rule is now
structural in `draw_pill_row()`: **a row with a sub-label is a list item** (SSID left, signal right) and stays
left-aligned; **a row without one is a button** and is centred in its pill.

That also centres the WiFi screen's two action rows (`SCAN / RECONNECT`, `FORGET NETWORK`) for free, which is
consistent rather than a side effect — they were always buttons drawn with list alignment. Verified on the panel:
the update screen's button is centred, and the WiFi screen's `STATUS` / `SSID` / `IP` rows (drawn by `draw_kv`,
not this helper) are untouched.

Note on the S3 build: it failed once with `libesp_driver_dma.a: No such file` after the mbedTLS/cert-bundle
config landed — a stale build tree, not a code error. Removing `build/pico-e32-fake08/makerfabs-ili9488-r1` and
rebuilding fixed it. Same family as the `sdkconfig` staleness in §5: config changes need the generated tree
dropped.

### 11. S3 — updated end to end, and a cross-board guard

**S3 OTA verified on hardware**, the acceptance item that was outstanding:

```
App version: 09f8874-dirty                       <- running
wifi: autoconnect -> Tukang Ketoprak
sta ip: 192.168.7.228
ota: manifest: ota-test-s3 for esp32s3 (1628192 bytes)
ota: target slot 'ota_1' @0x420000 (4096 KB)
ota: verified 1628192 bytes, boot slot -> 'ota_1'
rst:0xc (RTC_SW_CPU_RST)
App version: ota-test-s3                         <- NEW FIRMWARE RUNNING
ota: new image confirmed good (ESP_OK) — rollback cancelled
```

1.63 MB in **9.1 s** (~180 KB/s on the S3's native radio, against ~218 KB/s on the P4 over the C6 — the remoted
radio is not the slower one, which is mildly surprising and worth remembering).

**Gap found by doing the second board: the manifest had no chip field.** Two boards, one endpoint, and nothing
stopped an S3 downloading a P4 image. IDF *does* reject a wrong chip ID — but in the **bootloader**
(`bootloader_common_loader.c`: `mismatch chip ID, expected %d, found %d`), i.e. only at the next boot. The board
would burn the whole transfer, write it, switch slots, fail to boot and roll back. Recoverable, but a scary
reboot instead of a clear message.

So `target` is now a **required** manifest field, checked against `CONFIG_IDF_TARGET` in `ota_check()` before
anything is downloaded, with the menu saying `UPDATE IS FOR ANOTHER BOARD`. The positive path is verified — the
S3 run above parsed and matched `"target":"esp32s3"` (`manifest: ota-test-s3 for esp32s3`). **The negative path
is not yet verified on hardware**: repeated attempts to drive it were lost to serial-port contention, so
"mismatch is refused" is currently reasoning plus a passing parse, not a HITL result.

Manifest format is now:
`{"target","version","build","url","sha256","size"}`.

### 12. The remaining acceptance items — and two vacuous "passes"

**Wrong-chip refusal — verified.** Served the S3 a `"target":"esp32p4"` manifest:

```
E ota: manifest targets 'esp32p4' but this is 'esp32s3' — refusing
```

Refused at *check* time, with no `target slot` line — nothing was downloaded or written. That is the point of
checking in `ota_check()` rather than leaving it to the bootloader.

**Power-pull mid-download — verified, but only on the third attempt, and the first two were worthless.** The
first two runs reset the board and it came back fine, which *looked* like a pass. It wasn't: the S3 was already
running `ota-test-s3` and the manifest offered `ota-test-s3`, so `ota_is_newer()` correctly said "up to date" and
**no download ever started**. Resetting an idle board proves nothing. The tell was a missing `target slot` line —
worth catching, because a green result from a test that never exercised the path is worse than a red one.

Fixed by offering `ota-test-s3b` (same bytes, different version string — `ota_is_newer` is an inequality, which
is exactly why that works) and asserting the write was in flight before pulling the plug:

```
FLASH WRITE IN FLIGHT: True
ota: target slot 'ota_0' @0x20000 (4096 KB)
<<< RESET WHILE WRITING >>>
rst:0x1 (POWERON)
App version: ota-test-s3      <- previous firmware, untouched
sdcard_spi: SD mounted at /sdcard
carousel: carousel layout ...
```

A reset ~3 s into the flash write leaves the board fully bootable on the old firmware.

**TLS — verified.** Pointed a build at a real public HTTPS endpoint
(`https://raw.githubusercontent.com/jtothebell/fake-08/master/README.md`, read-only, nothing published):

```
E ota: manifest missing a required field
```

That error is the success signal: it can only be reached after a completed TLS handshake, certificate validation
against the bundled CA roots, HTTP 200, and a body read. A TLS failure would have errored earlier, at
`manifest open`. **Still untested:** a full ~1.6 MB image transfer over TLS — only the small manifest fetch went
over HTTPS. The image download uses the same `esp_http_client` config, so it is the same code path, but that is
reasoning rather than a measurement.

### 13. Acceptance status — final

| criterion | status |
|---|---|
| (a) end-to-end update, version visibly changes | ✅ **both boards** (P4 `ota-test-2`, S3 `ota-test-s3`) |
| (b) corrupted image rejected, old firmware still boots | ✅ verified (P4) |
| (c) power-pull mid-download leaves the board bootable | ✅ verified (S3, write confirmed in flight) |
| (d) radio + SD host released on every exit path | ✅ observed across the failure paths exercised |
| (e) no-network case reports cleanly | ✅ |
| wrong-chip image refused | ✅ verified (S3) |
| TLS | ✅ handshake + cert validation; ⬜ full image transfer over TLS |

Throughput: **P4 218 KB/s** (1.66 MB / 7.6 s, over the C6), **S3 180 KB/s** (1.63 MB / 9.1 s, native radio).

### 14. Board state

Both boards reflashed to the shipped touch build (`-D LAUNCHER=1`) with **no manifest URL configured**, so
SYSTEM UPDATE reports `NO UPDATE URL IN BUILD` rather than pointing at a bench laptop. The test server, images
and manifests live only in the scratchpad and are not committed.

### 15. A bench endpoint: `tools/ota_server.py`

Serving the update by hand (build → copy → `sha256sum` → hand-write JSON → `python3 -m http.server`) is exactly
the process that produced the vacuous power-pull "passes" in §12 and the stale-hash confusion before it. Wrapped
it up as `tools/ota_server.py`.

The design point: **the manifest is derived from the image, never typed.**

| field | source |
|---|---|
| `target` | chip id in the image header (`esp_image_header_t`, uint16 @ `0x0C`) |
| `version` | app descriptor (`esp_app_desc_t.version` @ `0x30`) |
| `sha256` | hash of the file on disk |
| `size` | length of the file on disk |

A manifest that disagrees with its binary is precisely what the device-side checks are for — wrong `target` is
refused up front, wrong `sha256`/`size` aborts after a wasted transfer. Deriving all four makes that class of
mistake unauthorable. It also advertises the machine's **LAN IP**, not `127.0.0.1`, since a board cannot reach
localhost, and it prints the two gotchas that cost time here: the generated `sdkconfig` must be deleted for
`sdkconfig.defaults` to apply, and re-serving the running build is *correctly* `ALREADY UP TO DATE`.

Verified rather than assumed:

- Parsed both real binaries — `esp32p4` / `esp32s3` correctly distinguished, versions matching what the boards
  report (`81a7673-dirty` / `81a7673`), and both `sha256` values cross-checked against `sha256sum`.
- **A board consumed its output**: pointed an S3 build at the generated URL and got
  `ota: manifest: 81a7673 for esp32s3 (1438624 bytes)` — fetch, parse, target match and version compare all
  against the script's own manifest.

Lives in `tools/` alongside the other bench utilities (`fb_screenshot.py`, `capture_frame.sh`) — one home
for this kind of thing rather than a second script directory.

### 16. Review pass — five findings, one of them serious

**1. `ota_mark_valid()` sat on a path many boots never reach — MAJOR.** With rollback armed, an OTA'd image
boots `PENDING_VERIFY` and reverts unless it confirms itself. The only confirm call lived inside
`carousel_launcher_run()`, which `main.cpp` invokes **only when `sd_ret == ESP_OK`** (and not under
`FORCE_FLASH_CART`, and not if the PSRAM scratch alloc fails). So: update succeeds → reboot → card absent or
failed to mount → launcher never runs → **the good update silently rolls back**. Worse, the retry is then
blocked: with the app still pending-verify `esp_ota_begin()` returns `ESP_ERR_OTA_ROLLBACK_INVALID_STATE`,
surfacing only as a generic "UPDATE FAILED".

Moved to `app_main`, after board + SD bring-up and before the cart ladder — deliberately not the first line
either, so an image that panics during bring-up can still roll back. Verified by the log ordering flipping:

| | before | after |
|---|---|---|
| `carousel layout` | 1201 ms | 1849 ms |
| `new image confirmed good` | 1324 ms | **1773 ms** |

Confirm now precedes the launcher, so a card-less boot still confirms.

**2. The `OTA_INSECURE` escape hatch was dead code.** `-D OTA_INSECURE=1` becomes a CMake cache variable, and
only `main/CMakeLists.txt` forwards names into compile definitions — `components/ota` forwarded nothing. Against
a self-signed bench server the build would still validate against the CA bundle and fail, with the loud "never
ship this" warning never printing. Exactly the trap documented in §5, missed one directory over. Now forwarded,
and the CMake shouts when it is on.

**3. The confirm screen drew manifest-controlled strings with no width bound.** `draw_kv` right-aligns by string
length and nothing clips, so a long `version` (up to 47 chars) or `build` runs into its own label and, far
enough, hands a **negative x** to the panel blit. Same overlap bug fixed one screen over in §9 — the settings row
got a guard, the confirm screen did not. Added `fit_text()`, clamped to what the row actually holds.

**4. No redirect handling.** `esp_http_client_open` + `fetch_headers` does not follow 3xx (that is
`esp_http_client_perform`, which we cannot use — the body is streamed into flash). Real hosting redirects
constantly, so the first non-bench endpoint would have failed as a bare "not found". Both fetches now follow up
to 4 hops via `esp_http_client_set_redirection()`.

**5. The `for(;;)` in `run_update` was dead** — every path returned, so a transient AP hiccup dumped the user
back to Settings instead of letting them press CHECK again. Recoverable outcomes now `continue`.

Also confirmed sound by the review, worth recording: acquire/release is balanced on every `run_update` exit and
always precedes the `~SdHostLoan` remount; `esp_ota_abort` is reached exactly once per failure path; the
over-long-body check happens *before* the write; and `ota_server.py`'s header offsets and chip-id table are
correct.
