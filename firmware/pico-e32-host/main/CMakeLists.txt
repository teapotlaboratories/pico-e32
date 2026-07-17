# The board owns the display driver: compile boards/<BOARD>/board.cpp (selected by BOARD_DIR) and
# link LovyanGFX directly. No shared display component anymore. assets/ is on the include path so the
# optional on-panel Celeste demo can pick up CELESTE_* from the (gitignored) celeste_cart.h.
idf_component_register(
    SRCS "host_main.cpp" "${BOARD_DIR}/board.cpp"
    INCLUDE_DIRS "." "${BOARD_DIR}" "${CMAKE_CURRENT_LIST_DIR}/../../../assets"
    REQUIRES z8lua LovyanGFX driver esp_timer
)

# HG-4 on-panel demo: `make build APP=pico-e32-host ... DEFS='-D CELESTE_DEMO=1'` draws real Celeste
# rooms via map() instead of the trivial cart. Off by default.
if(DEFINED CELESTE_DEMO)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CELESTE_DEMO=${CELESTE_DEMO})
endif()
