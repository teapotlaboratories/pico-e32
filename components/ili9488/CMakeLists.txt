# Reusable ILI9488 display driver — two interchangeable backends, one public API (ili9488.h).
#
#   lovyangfx (default) — ili9488.cpp over LovyanGFX (submodule components/LovyanGFX). This is what
#                         the factory firmware for this board revision uses. Proven upright + correct
#                         colour + 393 fps this session.
#   esp_lcd             — ili9488_esp_lcd.c over ESP-IDF's built-in esp_lcd i80 bus. No 132 MB
#                         submodule, and FASTER (590 vs 393 fps, true zero-copy DMA). BUT its colour
#                         is broken on this board — every bright fill renders the same teal and a byte
#                         swap doesn't fix it; likely needs LovyanGFX's data-pin crossover, which is
#                         entangled with esp_lcd's command path (needs a logic analyzer to confirm).
#                         Kept for the comparison + future debugging, NOT default. See
#                         docs/worklog/2026-07-16-esp-lcd-vs-lovyangfx.md.
#
# Select the backend at build time (least-destructive: neither file is deleted):
#   make build ... ILI9488_BACKEND=esp_lcd     (the repo Makefile forwards this as -D ILI9488_BACKEND)
set(ILI9488_BACKEND "lovyangfx" CACHE STRING "ILI9488 driver backend: lovyangfx or esp_lcd")

if(ILI9488_BACKEND STREQUAL "esp_lcd")
    idf_component_register(
        SRCS "ili9488_esp_lcd.c"
        INCLUDE_DIRS "."
        REQUIRES esp_lcd driver
    )
    message(STATUS "ili9488: backend = esp_lcd (i80)")
else()
    idf_component_register(
        SRCS "ili9488.cpp"
        INCLUDE_DIRS "."
        REQUIRES LovyanGFX driver
    )
    message(STATUS "ili9488: backend = lovyangfx")
endif()
