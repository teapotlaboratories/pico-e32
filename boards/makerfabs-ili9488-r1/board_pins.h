/* Makerfabs ESP32-S3 Parallel TFT with Touch (ILI9488) 3.5" — **FIRST REVISION** pin map.
 *
 * THE ONE DEFINITION. Apps must not spell these numbers out themselves: a board fact copied into
 * application code is a fact that drifts, and this one drifts silently — the wrong map leaves the
 * panel backlit-white while DMA completes, framebuffer checksums match, and every diagnostic
 * reports success. That is not hypothetical; it cost this project two days.
 * See docs/worklog/2026-07-16-panel-rev1-pinmap.md.
 *
 * ┌──────────────────────────────────────────────────────────────────────────────────────┐
 * │ ⚠ THIS BOARD HAS TWO PIN MAPS. WHICH ONE DEPENDS ON THE PSRAM PART.                  │
 * │                                                                                       │
 * │   rev 1 (THIS UNIT)  N16R2 · 2 MB QUAD PSRAM  → 35/36/37 free  → LCD WR=35 DC=36 CS=37│
 * │   current rev        N16R8 · 8 MB OCTAL PSRAM → 35/36/37 taken → LCD WR=18 DC=17 CS=46│
 * │                                                                                       │
 * │ ESP32-S3 octal PSRAM occupies GPIO 35/36/37, so Makerfabs relocated the LCD when they  │
 * │ moved to the N16R8 part. Both maps are published by the vendor, for different boards,  │
 * │ with nothing marking which is which — their firmware/SD16_3.5/SD16_3.5.ino was even    │
 * │ edited IN PLACE across the revision. Check the revision, not just the vendor.          │
 * └──────────────────────────────────────────────────────────────────────────────────────┘
 *
 * Source of record (pinned): references/makerfabs-parallel-tft-lvgl-lgfx @ 6d4b014,
 *   main/LGFX_MakerFabs_Parallel_S3.hpp  — WR 35, RS 36, CS 37, RD 48, 40 MHz, dlen_16bit.
 * Full context + the other revision's map: docs/reference/pico-e32-makerfabs-boards.md
 *
 * Reached by the build via -D BOARD_DIR (see the repo Makefile), so switching BOARD switches
 * the pin map with it.
 */
#pragma once

/* The macros below are deliberately BOARD-AGNOSTIC (ILI9488_*, not MAKERFABS_*): apps must not
 * name a vendor or revision. Which board_pins.h gets included is decided by BOARD at build time
 * (-D BOARD_DIR, see the repo Makefile), so an app says "the ILI9488 pins for whatever board is
 * selected" and stays portable. Any other ILI9488 board provides the same macro from its own
 * boards/<board>/board_pins.h.
 *
 * Panel geometry (native, portrait). */
#define ILI9488_H_RES 320
#define ILI9488_V_RES 480

/* Drop-in initialiser for ili9488_config_t (components/ili9488). Usage:
 *
 *     ili9488_config_t cfg = {
 *         ILI9488_PINS,
 *         .max_transfer_bytes = ...,     // the only app-specific field
 *     };
 *
 * Designators are listed in ili9488_config_t's declaration order: C++ requires that, and the
 * app appends .max_transfer_bytes last. Keep this macro in struct order if the struct changes.
 *
 * RD (48) must be driven HIGH on this 8080 bus — the driver does that; it is a pin, not a
 * strobe we generate. CS is a real GPIO on rev 1 (37); it is NOT tied low, despite what the
 * newer board's vendor config implies.
 *
 * .mirror_y — THE GLASS IS MOUNTED UPSIDE-DOWN on this board: at the controller's native scan
 * direction GRAM row 0 lands at the BOTTOM, so everything renders vertically mirrored. A board
 * fact, not an ILI9488 or driver fact, so it lives here and the driver only applies it.
 * Measured via the L-pattern, not assumed: docs/worklog/2026-07-16-yflip-and-gate1-fps.md.
 *
 * Note the vendor's own rev-1 config does NOT set this — because their demo only ever ran
 * setRotation(1) (landscape). Rotation 0 is untested upstream, so "matches the vendor" was
 * never evidence it renders upright. Same trap as the pin map above, one layer in.
 */
#define ILI9488_PINS                                        \
    .pin_wr = 35, .pin_dc = 36, .pin_cs = 37, .pin_bl = 45,           \
    .pin_rd = 48,                                                     \
    .data_pins = { 47, 21, 14, 13, 12, 11, 10, 9,                     \
                    3,  8, 16, 15,  7,  6,  5, 4 },                   \
    .pclk_hz = 40 * 1000 * 1000,                                      \
    .h_res = ILI9488_H_RES,                                 \
    .v_res = ILI9488_V_RES,                                 \
    .mirror_y = true,                                                 \
    .swap_color_bytes = true
