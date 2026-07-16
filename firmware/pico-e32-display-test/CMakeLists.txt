# pico-e32-display-test — Phase-0 Gate #1: ILI9488 i80 scaled blit + FPS.
# Build from the repo root:  make build APP=pico-e32-display-test BOARD=makerfabs-ili9488-r1
cmake_minimum_required(VERSION 3.16)

# Shared components live at the repo root (none needed today; kept for parity).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pico_e32_display_test)
