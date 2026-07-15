# Reusable ILI9488 display driver (esp_lcd i80). See ili9488.h.
idf_component_register(
    SRCS "ili9488.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver
)
