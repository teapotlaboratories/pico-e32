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

# ── P4: defer esp-hosted's boot auto-init ───────────────────────────────────────────────────────────────
# The ESP32-P4 has no native radio; WiFi runs on an on-board ESP32-C6 via espressif/esp-hosted (SDIO). That
# component auto-inits itself from a C global constructor (host/port/.../port_esp_hosted_host_init.c:
# `__attribute__((constructor))` -> `ESP_ERROR_CHECK(esp_hosted_init())`), i.e. BEFORE app_main. In this
# firmware that hang the P4 during the C6 SDIO bring-up: the H_SDIO_DRV rx task starts and the boot stalls,
# app_main is never reached. We instead bring the link up from app_main after board bring-up, in
# components/wifi/wifi_manager.c (esp_hosted_init() + esp_hosted_connect_to_slave()), which is reliable and
# idempotent. The component manager fetches esp-hosted PRISTINE into the (git-ignored) managed_components/
# tree; we transform that one call here at configure time rather than committing a modified vendor copy, so
# the vendored tree stays byte-identical to upstream and the change re-applies after any clean/re-fetch.
if(IDF_TARGET STREQUAL "esp32p4")
    set(_hosted_ctor "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__esp_hosted/host/port/esp/freertos/src/port_esp_hosted_host_init.c")
    if(EXISTS "${_hosted_ctor}")
        file(READ "${_hosted_ctor}" _hosted_src)
        if(NOT _hosted_src MATCHES "pico-e32: esp_hosted_init")
            string(REPLACE
                "\tESP_ERROR_CHECK(esp_hosted_init());"
                "\t/* pico-e32: esp_hosted_init() deferred to app_main (components/wifi/wifi_manager.c) — running\n\t * it here, from a global constructor before app_main, hangs the P4 in the C6 SDIO bring-up. See the\n\t * project CMakeLists for why this is transformed at build time. */\n\t/* ESP_ERROR_CHECK(esp_hosted_init()); */"
                _hosted_patched "${_hosted_src}")
            if(_hosted_patched STREQUAL _hosted_src)
                message(WARNING "esp-hosted defer-init patch: constructor call not found — upstream may have "
                                "changed. Verify WiFi still boots on the P4 (see components/wifi).")
            else()
                file(WRITE "${_hosted_ctor}" "${_hosted_patched}")
                message(STATUS "esp-hosted: deferred boot auto-init out of the C constructor (P4 C6 SDIO).")
            endif()
        endif()
    endif()
endif()
