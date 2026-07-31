# pico-e32-fake08 — the fake-08 port: fake-08's PICO-8 runtime (components/fake08) rendering on the
# board panel. Draw-only milestone. Build from the repo root:
#   make build APP=pico-e32-fake08 BOARD=makerfabs-ili9488-r1
cmake_minimum_required(VERSION 3.16)

# Shared components (fake08 runtime, z8lua VM, LovyanGFX) live at the repo root. The board's display
# driver (boards/<BOARD>/board.cpp) is compiled into main, not a component.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

# ── Build mode ────────────────────────────────────────────────────────────────────────────────────────
# `-D PLAYTEST=1` bundles the standard play-test flags (serial input + per-frame telemetry + frame-exact
# input hold + fps HUD) so you don't hand-set the soup; the default is the shipped TOUCH build. Set before
# project() so the input/fake08 component CMakeLists read them.
#   touch (shipped):  make build APP=pico-e32-fake08 ... DEFS='-D CELESTE=1 -D FORCE_FLASH_CART=1'
#   play-test:        make build APP=pico-e32-fake08 ... DEFS='-D CELESTE=1 -D FORCE_FLASH_CART=1 -D PLAYTEST=1'
# PLAYTEST FORCE-sets its flags (so they win over the cache); the shipped default only sets touch when no
# backend was chosen — so specialized builds that pass their own backend + flags still work unchanged (the
# racer: INPUT_BACKEND=serial + RACER + ...; the closed-loop TWIN: INPUT_BACKEND=scheduled). The game is
# ALWAYS flush-to-top (CENTER_GAME was removed), so no build can overlap the game with the deck.
# NOTE: idf.py's -D defines are STICKY in the CMakeCache — `make fullclean` when switching build configs.
if(PLAYTEST)
    set(INPUT_BACKEND "serial" CACHE STRING "input backend" FORCE)
    set(TELEMETRY         1 CACHE STRING "playtest: per-frame state + fps over serial" FORCE)
    set(INPUT_HOLD_FRAMES 1 CACHE STRING "playtest: frame-exact input replay" FORCE)
    set(SHOW_FPS          1 CACHE STRING "playtest: on-screen fps HUD" FORCE)
elseif(NOT DEFINED INPUT_BACKEND OR "${INPUT_BACKEND}" STREQUAL "")
    set(INPUT_BACKEND "touch" CACHE STRING "input backend" FORCE)   # shipped default
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pico_e32_fake08)
