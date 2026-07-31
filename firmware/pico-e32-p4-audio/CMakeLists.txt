# pico-e32-p4-audio — minimal ES8311 tone test for the Guition JC4880P443C (ESP32-P4). No fake-08, no
# display, no board.cpp: it brings up ONLY the ES8311 codec + an I2S TX channel and plays a sine, to
# isolate the audio path (codec / amp / PA-enable) from the fake-08 audio wiring. Build from the repo root:
#   make build APP=pico-e32-p4-audio BOARD=guition-jc4880p443c
# Sweep a candidate speaker-amp enable pin:  ... DEFS='-D PA_GPIO=NN'
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pico_e32_p4_audio)
