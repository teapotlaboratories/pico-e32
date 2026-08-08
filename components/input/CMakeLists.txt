# Compile-time-selectable input source for the fake-08 Host (ESP32Host::scanInput). Exactly ONE backend
# is compiled, chosen by INPUT_BACKEND (default: stub, so a FRESH build is unchanged — note the value
# persists in CMakeCache once set via -D, so re-pass it or `make fullclean` to switch). Switch it with:
#   make build APP=pico-e32-fake08 ... DEFS='-D INPUT_BACKEND=serial'   # stub | serial | touch | i2c | scheduled
# See docs/runtime/pico-e32-fake08-input.md. Auto-discovered via the app's EXTRA_COMPONENT_DIRS.
if(NOT DEFINED INPUT_BACKEND OR "${INPUT_BACKEND}" STREQUAL "")
    set(INPUT_BACKEND "stub")
endif()

set(_input_src "input_${INPUT_BACKEND}.c")
# input_exit.c is compiled with EVERY backend: the hold-MENU-to-exit gesture (IN-6) belongs to the device,
# not to one input source, and each backend calls input_exit_check() from inside its own input_poll().
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/${_input_src}")
    message(FATAL_ERROR "input: unknown INPUT_BACKEND='${INPUT_BACKEND}' (no ${_input_src}); use stub|serial|touch|i2c|scheduled")
endif()

# REQUIRES esp_driver_uart: the serial + scheduled backends include driver/uart.h. ESP-IDF v6 split the
# monolithic `driver` component apart, so a component must require the SPECIFIC esp_driver_* that provides
# its header (esp_driver_uart exists on v5.3+ too, so this stays valid on v5.4.2). The touch/i2c backends
# pull no driver/ header directly (touch reaches the FT6236 via board_touch_* externs). Harmless for stub.
# esp_driver_usb_serial_jtag: the serial backend uses it as its transport when the console is native
# USB-Serial-JTAG (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG — the ESP32-P4 board); the S3 (UART console) links it
# but never calls it. Both targets support the peripheral, so requiring it unconditionally is harmless.
idf_component_register(
    SRCS "${_input_src}" "input_exit.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../assets"
    REQUIRES esp_driver_uart esp_driver_usb_serial_jtag esp_system esp_timer
)
# PRIV_INCLUDE_DIRS assets/: the touch backend (input_touch.c) renders O/X/MENU labels from
# assets/pico8_font.h via the portable deck. Private — only this component's own sources use it.
message(STATUS "input: backend '${INPUT_BACKEND}' (${_input_src})")

# Opt-in override for the serial backend's auto-release hold length (game-update frames per byte).
# `... DEFS='-D INPUT_BACKEND=serial -D INPUT_HOLD_FRAMES=1'` gives frame-exact input for an automated,
# frame-synced driver; the default (6) is unchanged for manual single-key use.
if(DEFINED INPUT_HOLD_FRAMES)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE INPUT_HOLD_FRAMES=${INPUT_HOLD_FRAMES})
endif()
