# 2026-08-06 — P4 SD back on SDMMC, host handed to the radio on demand (`WC-6` step 1)

Goal: recover the SD throughput lost when the P4's card was moved to SPI. That move was forced by the C6 radio
needing the single SDMMC host while WiFi was always-on; `WC-5` made the radio on-demand, so the host is free
almost all the time and the SD can have it back.

This is **step 1** of `WC-6`: SD on SDMMC by default, handed over to the radio for the duration of a WiFi
session. Step 2 (SD on SPI *simultaneously* with the radio, for downloads that need both) waits until `WC-4`
lands something that actually needs it.

## TL;DR

- **Cover-art load: 64.0 ms → 39.4 ms median, a 38% cut** (mean 66.2 → 38.5 ms), measured in the shipped build on
  the same folder and method.
- **The SDMMC host is handed over at runtime**, verified end to end in the launcher: SD mounted 4-bit at boot →
  open Settings → WIFI → SD unmounted and host released → `Identified slave [esp32c6]` → radio up → leave the
  screen → radio down → **SD remounted in 44 ms** → carousel reads 19 folders / 402 entries off it again.
- **This is the loading-time win that `WC-5` did not deliver.** The on-demand radio work was measured and moved
  nothing (64.0 ms either way); it was, however, the *precondition* for this — it is what freed the host.
- One trap cost a boot loop; see §3.

## 1. Why the SD can have the host back

The P4 has one SDMMC host: slot 0 is the TF card, slot 1 is the on-board C6. They cannot be initialised at the
same time, which is why the SD went to SPI in the first place — with an always-on radio there was no other
option. Since `WC-5` the radio is off unless something acquires it, so for the overwhelming majority of the time
(browsing, playing) nothing wants slot 1.

The remaining question was whether the host can be *handed over* rather than merely claimed once. It can — the
original finding was only about simultaneous init. Sequence, measured:

```
board.p4: SD mounted at /sdcard (29820MB, 4-bit, CLK43 CMD44 D0-3=39/40/41/42, LDO VO4)
board.p4: SD unmounted, SDMMC host released -> ESP_OK        <- entering Settings->WIFI
transport: Identified slave [esp32c6]
wifi: radio up (internal heap 236615, tasks 15)
wifi: radio down (internal heap 362255, tasks 9)             <- leaving the screen
board.p4: SD mounted at /sdcard (29820MB, 4-bit, ...)        <- 44 ms later
carousel: games: 19 entries in /sdcard                       <- card fully usable again
carousel: cd /sdcard/[Action-Adventure] (402 entries)
```

## 2. Measurements

Cover-art load (read + PNG decode), same folder and method, scrolling the carousel to force uncached loads:

| SD bus | n | min | median | mean | max |
|---|---|---|---|---|---|
| **SDMMC 4-bit @40 MHz** (now) | 15 | 31.6 | **39.4** | 38.5 | 45.9 ms |
| SPI 1-bit @20 MHz (before) | 10 | 49.9 | **64.0** | 66.2 | 82.4 ms |

**−38% median.** The mechanism, from the read/decode split measured separately on a ~38 KB average cover:

| | read | throughput | decode |
|---|---|---|---|
| SDMMC 4-bit | **3.6 ms** | **10.20 MB/s** | 28.9 ms |
| SPI 20 MHz | 26.3 ms | 1.43 MB/s | 29.1 ms |

Read is **7.1× faster**; decode is unchanged and is now ~75% of what remains. Further gains have to come from the
decoder, not the bus — the bus is no longer the bottleneck.

## 3. The trap: do not deinit the host yourself

First attempt at the unmount added an explicit `sdmmc_host_deinit()` after the VFS unmount. That **panics the
board into a boot loop** — `Guru Meditation Error: Core 0 panic'ed (Unknown)`, `MCAUSE 0x1f`, immediately after
the unmount log.

Cause: `esp_vfs_fat_sdcard_unmount()` **already** releases the host and frees the card. IDF
`components/fatfs/vfs/vfs_fat_sdmmc.c`, `unmount_card_core()`:

```c
if (pdrv_num == 1) {
    call_host_deinit(&card->host);
    free(card);
}
```

So the explicit call is a double-deinit through freed state. Unmount, and nothing else. The comment on
`board_sd_unmount()` says so, so it doesn't get re-added.

Also deliberate: the **LDO VO4 rail stays powered** across the handover. It powers the card itself, is needed
again on remount, and tearing it down costs ~300 ms of settling.

## 4. Shape of the change

- `boards/guition-jc4880p443c/board.{h,cpp}` — back to `BOARD_HAS_SDMMC` + `board_sd_mount()`, plus a new
  `board_sd_unmount()` that releases the host. The SPI seam (`board_sd_config`) is gone from this board; step 2
  restores it as the *network-session* mode.
- `main.cpp` — the `#elif BOARD_HAS_SDMMC` mount branch is back.
- `carousel_launcher.cpp` — `run_wifi()` unmounts the SD before `wifi_mgr_acquire()` and remounts after
  `wifi_mgr_release()`, including on the acquire-failure path so a dead radio doesn't cost the card.

**Sequencing lives in the app, not the wifi component.** `wifi_manager` still knows nothing about storage and the
board owns the driver; the launcher is the only place that knows both. Safe because nothing on the WiFi screen
touches storage and the launcher holds no open file handles — cover art is decoded into PSRAM at browse time,
not streamed.

**The S3 is untouched** — native radio, no shared host, still SPI.

## 5. Commands run (reproduce)

```sh
make flash APP=pico-e32-fake08 BOARD=guition-jc4880p443c PORT=/dev/ttyACM0 \
     DEFS="-D LAUNCHER=1 -D INPUT_BACKEND=serial"     # menu-driving test build
make flash APP=pico-e32-fake08 BOARD=guition-jc4880p443c PORT=/dev/ttyACM0 DEFS="-D LAUNCHER=1"   # shipped
```

Handover test: boot, `d` `o` `d` to Settings → WIFI, `o` (unmount + radio up), `x` (radio down + remount), then
`x` `u` `o` back into Games to prove the card still reads.

## 6. Board state

Both boards left on the shipped touch build (`-D LAUNCHER=1`). P4 on SDMMC; S3 unchanged on SPI.

## 7. Next

`WC-6` step 2 — SD on SPI *while* the radio is up, for downloads that need storage and network at once. Already
proven as a pairing (it is what shipped before this change); needs the board to carry both drivers and a
mode-switch seam, plus a no-open-file-handles rule across the switch. Blocked on nothing except `WC-4` producing
a consumer that needs it.
