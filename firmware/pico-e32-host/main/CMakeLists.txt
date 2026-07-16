# The board owns the display driver: compile boards/<BOARD>/board.cpp (selected by BOARD_DIR) and
# link LovyanGFX directly. No shared display component anymore.
idf_component_register(
    SRCS "host_main.cpp" "${BOARD_DIR}/board.cpp"
    INCLUDE_DIRS "." "${BOARD_DIR}"   # boards/<BOARD>/board.h
    REQUIRES z8lua LovyanGFX driver esp_timer
)
