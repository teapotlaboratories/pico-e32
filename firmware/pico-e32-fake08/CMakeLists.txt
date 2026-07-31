# pico-e32-fake08 — the fake-08 port: fake-08's PICO-8 runtime (components/fake08) rendering on the
# board panel. Draw-only milestone. Build from the repo root:
#   make build APP=pico-e32-fake08 BOARD=makerfabs-ili9488-r1
cmake_minimum_required(VERSION 3.16)

# Shared components (fake08 runtime, z8lua VM, LovyanGFX) live at the repo root. The board's display
# driver (boards/<BOARD>/board.cpp) is compiled into main, not a component.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

# ── Build mode: one flag instead of the play-test flag soup ───────────────────────────────────────────
# `-D PLAYTEST=1` selects the harness/measurement build: serial input, per-frame telemetry, frame-exact
# input hold, and the on-screen fps HUD — but the SAME flush-to-top game layout as the shipped build (it
# just leaves the deck band empty; no touch input). The default (or `-D PLAYTEST=0`) is the shipped TOUCH
# build: on-screen deck, game flush to the top. The derived flags are both SET (playtest) and
# UNSET (touch) here, so switching modes can never leave a stale, half-applied combination in the CMake
# cache — that is what produced the centred-game-overlapping-the-deck bug. Set before project() so the
# component CMakeLists (input, fake08) read them. Both INPUT_BACKEND and the derived flags are FORCE-set
# per mode (not `if(NOT DEFINED)`), because a plain default loses to a value already in the cache — which is
# exactly the stale-cache trap this flag exists to prevent. The shipped build is always touch; PLAYTEST is
# the OPEN-LOOP serial play-test. (The closed-loop TWIN's `scheduled` backend stays a separate expert flow
# with its own flags — see test/playtest/celeste/fc_device.py.)
#   touch (shipped):  make build APP=pico-e32-fake08 ... DEFS='-D CELESTE=1 -D FORCE_FLASH_CART=1'
#   play-test:        make build APP=pico-e32-fake08 ... DEFS='-D CELESTE=1 -D FORCE_FLASH_CART=1 -D PLAYTEST=1'
# NOTE: PLAYTEST itself is a cached -D — pass `-D PLAYTEST=0` (or `make fullclean`) to switch back to touch.
if(PLAYTEST)
    set(INPUT_BACKEND "serial" CACHE STRING "input backend" FORCE)
    set(TELEMETRY         1 CACHE STRING "playtest: per-frame state + fps over serial" FORCE)
    set(INPUT_HOLD_FRAMES 1 CACHE STRING "playtest: frame-exact input replay" FORCE)
    set(SHOW_FPS          1 CACHE STRING "playtest: on-screen fps HUD" FORCE)
else()
    set(INPUT_BACKEND "touch" CACHE STRING "input backend" FORCE)   # shipped build is always touch
    unset(TELEMETRY CACHE)
    unset(INPUT_HOLD_FRAMES CACHE)
    unset(SHOW_FPS CACHE)
endif()
# The game keeps the SAME flush-to-top layout in both modes (the play-test build just leaves the deck band
# empty — no touch input). CENTER_GAME is deliberately NOT driven by PLAYTEST; it's unset here so a stale
# cached one can never resurface (it once left a centred game overlapping the deck).
unset(CENTER_GAME CACHE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pico_e32_fake08)
