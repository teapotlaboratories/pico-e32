# pico-e32 firmware — convenience wrapper around ESP-IDF's idf.py.
#
# Board-specific sdkconfig defaults live in boards/<BOARD>/; the app's own
# sdkconfig.defaults is layered on top. Build output is out-of-source under
# build/<APP>/<BOARD>/. Examples:
#
#   make build                                    # default app + board (below)
#   make build APP=pico-e32-display-test
#   make flash-monitor BOARD=makerfabs-ili9488 PORT=/dev/ttyACM0
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
BOARD    ?= makerfabs-ili9488
APP_DIR  := $(CURDIR)/firmware/$(APP)
BOARD_DIR := $(CURDIR)/boards/$(BOARD)

# Board overlay first (owns CONFIG_IDF_TARGET + PSRAM), then the app's own config.
SDKCONFIG_DEFAULTS := $(BOARD_DIR)/sdkconfig.defaults$(if $(wildcard $(APP_DIR)/sdkconfig.defaults),;$(APP_DIR)/sdkconfig.defaults)

# Out-of-source build dir, per app+board, so switching boards never reuses a
# stale sdkconfig.
BUILD_DIR := $(CURDIR)/build/$(APP)/$(BOARD)

IDF := source "$(IDF_PATH)/export.sh" >/dev/null 2>&1 && cd "$(APP_DIR)" && \
       idf.py -B "$(BUILD_DIR)" \
              -D SDKCONFIG="$(BUILD_DIR)/sdkconfig" \
              -D SDKCONFIG_DEFAULTS="$(SDKCONFIG_DEFAULTS)"

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
