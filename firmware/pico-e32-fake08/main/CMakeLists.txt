# app_main boots fake-08's runtime (components/fake08) and drives its own Vm::GameLoop; the board owns
# the display. board.cpp (selected by BOARD_DIR) is compiled into main, same as the other apps.
idf_component_register(
    SRCS "main.cpp" "${BOARD_DIR}/board.cpp"
    INCLUDE_DIRS "." "${BOARD_DIR}" "${CMAKE_CURRENT_LIST_DIR}/../../../assets"
    REQUIRES fake08 z8lua LovyanGFX driver esp_timer sdcard_spi
)
# SD-over-SPI is now the board-agnostic components/sdcard_spi (fatfs/sdmmc/sdspi are its private
# concern); esp_driver_spi reaches board.cpp transitively via sdcard_spi's public REQUIRES.

# Opt-in Celeste build: `make build APP=pico-e32-fake08 ... DEFS='-D CELESTE=1'` embeds the real
# Celeste .p8 (the gitignored assets/celeste_p8.h) instead of the test cart. Off by default.
if(DEFINED CELESTE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CELESTE=${CELESTE})
endif()

# Opt-in fps measurement: `... DEFS='-D CELESTE=1 -D MEASURE_FPS=1'` runs an instrumented loop that
# times Step() vs drawFrame() over UART instead of fake-08's GameLoop. Off by default.
if(DEFINED MEASURE_FPS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MEASURE_FPS=${MEASURE_FPS})
endif()

# Opt-in dev/demo flag: `... DEFS='-D FORCE_FLASH_CART=1'` ignores any SD cart and runs the flash cart
# (e.g. the input-reactive test cart) even when the microSD holds a .p8. Off by default.
if(DEFINED FORCE_FLASH_CART)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FORCE_FLASH_CART=${FORCE_FLASH_CART})
endif()

# Opt-in serial position telemetry: `... DEFS='-D CELESTE=1 -D TELEMETRY=1'` runs a GameLoop variant that
# prints the Celeste player position + current room each frame over UART (via ExecuteLua/printh) for
# hardware-in-the-loop input solving/verification. Pairs with INPUT_BACKEND=serial. Off by default.
if(DEFINED TELEMETRY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TELEMETRY=${TELEMETRY})
endif()
