# app_main boots fake-08's runtime (components/fake08) and drives its own Vm::GameLoop; the board owns
# the display. board.cpp (selected by BOARD_DIR) is compiled into main, same as the other apps.
idf_component_register(
    SRCS "main.cpp" "${BOARD_DIR}/board.cpp"
    INCLUDE_DIRS "." "${BOARD_DIR}" "${CMAKE_CURRENT_LIST_DIR}/../../../assets"
    REQUIRES fake08 z8lua LovyanGFX driver esp_timer
)

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
