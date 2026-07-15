# pico-e32-luabench — Phase-0 Gate #2: z8lua interpreter throughput.
# Build from the repo root:  make build APP=pico-e32-luabench BOARD=makerfabs-ili9488
cmake_minimum_required(VERSION 3.16)

# Shared components (z8lua) live at the repo root, like rimba's components/.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pico_e32_luabench)
