idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "." "${BOARD_DIR}"   # boards/<BOARD>/board_pins.h
    REQUIRES ili9488 esp_timer
)
