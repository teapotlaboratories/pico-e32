# Compile-time-selectable input source for the fake-08 Host (ESP32Host::scanInput). Exactly ONE backend
# is compiled, chosen by INPUT_BACKEND (default: stub, so a FRESH build is unchanged — note the value
# persists in CMakeCache once set via -D, so re-pass it or `make fullclean` to switch). Switch it with:
#   make build APP=pico-e32-fake08 ... DEFS='-D INPUT_BACKEND=serial'   # stub | serial | touch | i2c
# See docs/runtime/pico-e32-fake08-input.md. Auto-discovered via the app's EXTRA_COMPONENT_DIRS.
if(NOT DEFINED INPUT_BACKEND OR "${INPUT_BACKEND}" STREQUAL "")
    set(INPUT_BACKEND "stub")
endif()

set(_input_src "input_${INPUT_BACKEND}.c")
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/${_input_src}")
    message(FATAL_ERROR "input: unknown INPUT_BACKEND='${INPUT_BACKEND}' (no ${_input_src}); use stub|serial|touch|i2c")
endif()

# REQUIRES driver: serial uses driver/uart.h; touch/i2c will use driver/i2c. Harmless for stub.
idf_component_register(
    SRCS "${_input_src}"
    INCLUDE_DIRS "."
    REQUIRES driver
)
message(STATUS "input: backend '${INPUT_BACKEND}' (${_input_src})")

# Opt-in override for the serial backend's auto-release hold length (game-update frames per byte).
# `... DEFS='-D INPUT_BACKEND=serial -D INPUT_HOLD_FRAMES=1'` gives frame-exact input for an automated,
# frame-synced driver; the default (6) is unchanged for manual single-key use.
if(DEFINED INPUT_HOLD_FRAMES)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE INPUT_HOLD_FRAMES=${INPUT_HOLD_FRAMES})
endif()
