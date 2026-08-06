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
