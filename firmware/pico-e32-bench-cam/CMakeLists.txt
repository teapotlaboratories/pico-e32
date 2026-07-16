# pico-e32-bench-cam — the bench capture camera (M5Stack Timer Camera F), NOT handheld firmware.
# Build/flash:  make build APP=pico-e32-bench-cam BOARD=m5stack-timer-cam
#               make flash APP=pico-e32-bench-cam BOARD=m5stack-timer-cam PORT=/dev/ttyUSB0
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pico_e32_bench_cam)
