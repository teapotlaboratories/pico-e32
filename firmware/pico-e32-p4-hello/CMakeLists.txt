# pico-e32-p4-hello — GP-2 proof-of-life for the Guition JC4880P443C (ESP32-P4). Dumps chip/PSRAM
# identity over serial and heartbeats; proves the P4 build -> flash -> boot path. Build from the repo root:
#   make build APP=pico-e32-p4-hello BOARD=guition-jc4880p443c
cmake_minimum_required(VERSION 3.16)

# Pull in ONLY the shared `input` component (board.cpp is compiled straight into main, not a component).
# EXTRA_COMPONENT_DIRS points at the single component dir, NOT the whole components/ tree: the `main`
# component implicitly depends on every discovered component, so pointing at components/ would drag
# z8lua/LovyanGFX/fake08/... in too (and input_stub.c trips -Werror here). Naming components/input keeps
# the build minimal — only the input backend the app actually uses is compiled.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/input")

# Default the input backend to `serial` for this P4 bring-up app (the component defaults to `stub`, whose
# input_stub.c trips -Werror here). Overridable: `make build ... DEFS='-D INPUT_BACKEND=serial'`.
if(NOT DEFINED INPUT_BACKEND)
    set(INPUT_BACKEND "serial" CACHE STRING "input backend: stub|serial|touch|i2c|scheduled")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pico_e32_p4_hello)
