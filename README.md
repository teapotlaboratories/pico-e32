# pico-e32

An open-source **PICO-8 handheld** built around the **ESP32-S3**.

The goal is a pocket-sized, DIY console for running [PICO-8](https://www.lexaloffle.com/pico-8.php)
carts (or a compatible fantasy-console runtime) on affordable ESP32-S3 hardware.

## Why ESP32-S3

- Dual-core 240 MHz Xtensa LX7 with vector/DSP instructions
- Ample PSRAM options for framebuffers and cart data
- Native USB (CDC/JTAG) for flashing and debugging
- Cheap, widely available, strong toolchain support (ESP-IDF / Arduino)

## Target hardware (draft)

| Subsystem | Candidate part | Notes |
|-----------|----------------|-------|
| MCU       | ESP32-S3 (N16R8) | 16 MB flash / 8 MB PSRAM |
| Display   | 128×128 ST7735 / 240×240 ST7789 | PICO-8 native res is 128×128 |
| Input     | D-pad + 2 face buttons | Matches PICO-8's 6-button layout |
| Audio     | I2S DAC + speaker | 4-channel PICO-8 sound |
| Power     | LiPo + USB-C charging | TP4056 or integrated PMIC |

## Status

🚧 Early planning. Nothing is finalized — hardware, firmware, and the runtime
approach are all open questions.

## Roadmap (rough)

- [ ] Lock down the ESP32-S3 module and display choice
- [ ] Breadboard prototype: display + input + audio
- [ ] Firmware bring-up (ESP-IDF)
- [ ] Fantasy-console runtime / Lua interpreter
- [ ] PCB design
- [ ] Enclosure

## License

TBD.
