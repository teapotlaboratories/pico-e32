# Self-contained: main.c inits the ES8311 + I2S itself (no board.cpp). esp_codec_dev comes via
# idf_component.yml (the component manager auto-adds it as a requirement).
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c esp_driver_i2s esp_driver_gpio esp_timer
)

# Optional: drive a candidate speaker-amp enable GPIO high before playing, to find the board's PA pin.
#   make build APP=pico-e32-p4-audio BOARD=guition-jc4880p443c DEFS='-D PA_GPIO=16'
if(DEFINED PA_GPIO)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PA_GPIO=${PA_GPIO})
endif()
