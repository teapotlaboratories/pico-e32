# Changelog

Notable changes to pico-e32. Newest first.

## Unreleased

- Repo restructured to the teapotlaboratories monorepo layout (rimba-style): `boards/`,
  root `components/`, `docs/{design-specification,reference,worklog}`, `firmware/<app>/`,
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
- **Docs:** decision-locked development plan (`docs/pico-e32-development-plan.md`) + HTML
  render; runtime-feasibility and silicon-decision studies under `design-specification/`;
  Makerfabs board pinouts under `reference/`.
- Initial `.ai/` agent guidance adapted for this embedded project.
