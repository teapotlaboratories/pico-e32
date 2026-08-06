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
