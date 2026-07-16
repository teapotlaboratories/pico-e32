# pico-e32 firmware — convenience wrapper around ESP-IDF's idf.py.
#
# Board-specific sdkconfig defaults live in boards/<BOARD>/; the app's own
# sdkconfig.defaults is layered on top. Build output is out-of-source under
# build/<APP>/<BOARD>/. Examples:
#
#   make build                                    # default app + board (below)
#   make build APP=pico-e32-display-test
#   make flash-monitor BOARD=makerfabs-ili9488-r1 PORT=/dev/ttyACM0
#
SHELL := /bin/bash

# The whole ESP-IDF SDK lives under vendor/: the framework (v5.4.2) at
# vendor/esp-idf (submodule), and the toolchain/tools at vendor/.espressif — so a
# checkout is self-contained. Override either to use a shared/global install.
IDF_PATH          ?= $(CURDIR)/vendor/esp-idf
export IDF_TOOLS_PATH ?= $(CURDIR)/vendor/.espressif
PORT              ?= /dev/ttyACM0

# Defaults: the verified Gate #2 app on the first device.
APP      ?= pico-e32-luabench
BOARD    ?= makerfabs-ili9488-r1
APP_DIR  := $(CURDIR)/firmware/$(APP)
BOARD_DIR := $(CURDIR)/boards/$(BOARD)

# BOARD_DIR is passed to CMake as well: boards/<BOARD>/board_pins.h holds the wiring, so
# switching BOARD switches the pin map with it. Apps add ${BOARD_DIR} to INCLUDE_DIRS.
# Board overlay first (owns CONFIG_IDF_TARGET + PSRAM), then the app's own config.
SDKCONFIG_DEFAULTS := $(BOARD_DIR)/sdkconfig.defaults$(if $(wildcard $(APP_DIR)/sdkconfig.defaults),;$(APP_DIR)/sdkconfig.defaults)

# Optional per-board make settings (e.g. BAUD). Included before the defaults below so a
# board can pin a value and still be overridden from the command line.
-include $(BOARD_DIR)/board.mk

# Flash/monitor baud. idf.py defaults to 460800, which some USB-serial bridges cannot
# sustain — the chip syncs at 115200 and then dies on the baud switch. Boards with a slow
# bridge pin this in their board.mk.
BAUD ?= 460800

# Out-of-source build dir, per app+board, so switching boards never reuses a
# stale sdkconfig.
BUILD_DIR := $(CURDIR)/build/$(APP)/$(BOARD)

# Secrets are passed on the command line, never stored in the tree. Used by
# pico-e32-bench-cam (the bench capture camera):
#   make flash APP=pico-e32-bench-cam BOARD=m5stack-timer-cam PORT=/dev/ttyUSB0 \
#        WIFI_SSID='my ssid' WIFI_PASS='my pass'
# Note: -D puts these in the *gitignored* build dir's CMakeCache.txt, so `make fullclean`
# after flashing if you don't want them lingering on disk.
WIFI_DEFS := $(if $(WIFI_SSID),-D WIFI_SSID="$(WIFI_SSID)") $(if $(WIFI_PASS),-D WIFI_PASS="$(WIFI_PASS)")

# Optional: pick the ILI9488 driver backend (components/ili9488 has two, one public API).
#   ILI9488_BACKEND=esp_lcd    -> ESP-IDF-native esp_lcd i80 (no LovyanGFX submodule)
#   ILI9488_BACKEND=lovyangfx  -> LovyanGFX (the default; leave unset for it)
ILI9488_DEFS := $(if $(ILI9488_BACKEND),-D ILI9488_BACKEND=$(ILI9488_BACKEND))

IDF := source "$(IDF_PATH)/export.sh" >/dev/null 2>&1 && cd "$(APP_DIR)" && \
       idf.py -B "$(BUILD_DIR)" -b "$(BAUD)" \
              -D SDKCONFIG="$(BUILD_DIR)/sdkconfig" \
              -D SDKCONFIG_DEFAULTS="$(SDKCONFIG_DEFAULTS)" \
              -D BOARD_DIR="$(BOARD_DIR)" \
              $(ILI9488_DEFS) \
              $(WIFI_DEFS)

.PHONY: help install build flash monitor flash-monitor clean fullclean menuconfig size erase

help:
	@echo "pico-e32 firmware build (ESP-IDF wrapper)"
	@echo "  APP=$(APP)  BOARD=$(BOARD)  PORT=$(PORT)"
	@echo "  make install       - one-time: install the esp32s3 toolchain into vendor/.espressif"
	@echo "  make build         - build APP for BOARD"
	@echo "  make flash         - flash APP to PORT"
	@echo "  make monitor       - serial monitor (Ctrl-] to exit)"
	@echo "  make flash-monitor - flash then monitor"
	@echo "  make size | menuconfig | clean | fullclean | erase"
	@echo ""
	@echo "  apps:   $(notdir $(wildcard firmware/pico-e32-*))"
	@echo "  boards: $(notdir $(wildcard boards/*))"

install:       ; "$(IDF_PATH)/install.sh" esp32s3
build:         ; $(IDF) build
flash:         ; $(IDF) -p "$(PORT)" flash
monitor:       ; $(IDF) -p "$(PORT)" monitor
flash-monitor: ; $(IDF) -p "$(PORT)" flash monitor
menuconfig:    ; $(IDF) menuconfig
size:          ; $(IDF) size
erase:         ; $(IDF) -p "$(PORT)" erase-flash
clean:         ; $(IDF) clean
fullclean:     ; rm -rf "$(BUILD_DIR)"
