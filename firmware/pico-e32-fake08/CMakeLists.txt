# pico-e32-fake08 — the fake-08 port: fake-08's PICO-8 runtime (components/fake08) rendering on the
# board panel. Draw-only milestone. Build from the repo root:
#   make build APP=pico-e32-fake08 BOARD=makerfabs-ili9488-r1
cmake_minimum_required(VERSION 3.16)

# Shared components (fake08 runtime, z8lua VM, LovyanGFX) live at the repo root. The board's display
# driver (boards/<BOARD>/board.cpp) is compiled into main, not a component.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pico_e32_fake08)
