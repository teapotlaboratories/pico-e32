idf_component_register(
    SRCS "host_main.cpp"
    INCLUDE_DIRS "." "${BOARD_DIR}"   # boards/<BOARD>/board_pins.h
    REQUIRES z8lua ili9488 esp_timer
)
