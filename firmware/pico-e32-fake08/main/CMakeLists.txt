# app_main boots fake-08's runtime (components/fake08) and drives its own Vm::GameLoop; the board owns
# the display. board.cpp (selected by BOARD_DIR) is compiled into main, same as the other apps.
idf_component_register(
    SRCS "main.cpp" "${BOARD_DIR}/board.cpp"
    INCLUDE_DIRS "." "${BOARD_DIR}"
    REQUIRES fake08 z8lua LovyanGFX driver esp_timer
)
