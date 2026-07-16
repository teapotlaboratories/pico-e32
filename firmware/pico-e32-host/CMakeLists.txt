# pico-e32-host — Phase-0 Gate #3: trivial cart end-to-end (minimal ESP32Host).
# Build from the repo root:  make build APP=pico-e32-host BOARD=makerfabs-ili9488-r1
cmake_minimum_required(VERSION 3.16)

# Shared components (z8lua runtime, LovyanGFX display library) live at the repo root. The board's
# display driver (boards/<BOARD>/board.cpp) is compiled into main, not a component.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pico_e32_host)
