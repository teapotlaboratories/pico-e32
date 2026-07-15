# pico-e32-bench-cam — the bench capture camera (ESP-EYE), NOT handheld firmware.
# Build/flash:  make build APP=pico-e32-bench-cam BOARD=esp-eye
#               make flash APP=pico-e32-bench-cam BOARD=esp-eye PORT=/dev/ttyUSB1
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pico_e32_bench_cam)
