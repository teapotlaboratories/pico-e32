# Reusable ILI9488 display driver — a thin wrapper over LovyanGFX. See ili9488.h.
# LovyanGFX (components/LovyanGFX, submodule pinned at 1.2.25) is what the factory firmware for
# this board revision uses. esp_lcd i80 was retried on the correct rev-1 pins (2026-07-16) and
# renders geometry fine but gets colour wrong — red and yellow come out blue/green — so it is
# rejected. See docs/worklog/2026-07-16-panel-rev1-pinmap.md.
idf_component_register(
    SRCS "ili9488.cpp"
    INCLUDE_DIRS "."
    REQUIRES LovyanGFX driver
)
