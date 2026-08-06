# Standalone ESP32-P4 + ESP32-C6 WiFi bring-up probe (project Gate 4 / WC-3). Isolates the C6/esp-hosted SDIO
# transport from fake-08 — same de-risking pattern as firmware/pico-e32-p4-audio. Build:
#   make build APP=pico-e32-p4-wifi BOARD=guition-jc4880p443c
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pico-e32-p4-wifi)
