# Changelog

Notable changes to pico-e32. Newest first.

## Unreleased

- Repo restructured to the teapotlaboratories monorepo layout (rimba-style): `boards/`,
  root `components/`, `docs/{reference,worklog}`, `firmware/<app>/`,
  and a top-level `Makefile` (`make build APP=<app> BOARD=<board>`).
- **ESP-IDF v5.4.2 vendored** as a shallow git submodule at `vendor/esp-idf` (pinned to the
  v5.4.2 commit); toolchain installs to `vendor/.espressif` (`make install`) so the whole SDK
  is self-contained. `IDF_PATH`/`IDF_TOOLS_PATH` default there.
- **Both Phase-0 apps compile clean for esp32s3** (`make build`). Building for the target
  caught + fixed a real portability bug in vendored `fix32.h` (plain `int` unconstructible
  where `int32_t == long` on xtensa); see `components/z8lua/LOCAL_PATCHES.md`.
- **Phase 0 firmware** on the 3.5" ILI9488 board:
  - `pico-e32-luabench` — z8lua (fix32) Gate #2 throughput benchmark. Host build verified;
    on-device (SRAM vs PSRAM heap) authored.
  - `pico-e32-display-test` — ILI9488 i80 scaled blit + FPS (Gate #1). Authored, unverified.
  - Vendored z8lua at `components/z8lua` (`jtothebell/z8lua`, compiled as C++).
- **fake-08 runtime port** (the primary goal — `firmware/pico-e32-fake08` + the vendored
  `components/fake08` fork; `source/` byte-identical to upstream):
  - **Draw-only milestone** — fake-08's own runtime boots on the ESP32-S3 and renders a `.p8` cart
    upright on the panel via its `Vm::GameLoop` (`ESP32Host` binding + the shared `components/z8lua`).
  - **SD cart loader → `components/sdcard_spi`** — mounts the onboard microSD and loads a `.p8`, falling
    back to the flash cart; a reusable, board-agnostic component (the board supplies wiring via
    `board_sd_config`, with an `owns_bus` flag for a shared display SPI bus). Verified on 32 GB SDHC.
  - **Input seam → `components/input`** — a compile-time-selectable input backend behind
    `ESP32Host::scanInput` (`INPUT_BACKEND` = stub|serial|touch|i2c). The serial (UART) backend is
    hardware-in-the-loop verified (input driven over the console UART moves the game on-panel); touch
    (FT6236) and i2c are skeletons.
  - **fake-08 nested** at `components/fake08/fake08` with a parent-repo wrapper `CMakeLists.txt`, so the
    fork stays a pure vendored tree.
- **Docs:** decision-locked development plan (`docs/pico-e32-development-plan.md`) + HTML
  render; runtime-feasibility and silicon-decision studies + Makerfabs board pinouts under
  `reference/`; a `docs/README.md` map of the doc tree.
- Initial `.ai/` agent guidance adapted for this embedded project.
