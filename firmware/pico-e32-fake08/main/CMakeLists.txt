# app_main boots fake-08's runtime (components/fake08) and drives its own Vm::GameLoop; the board owns
# the display. board.cpp (selected by BOARD_DIR) is compiled into main, same as the other apps.
idf_component_register(
    SRCS "main.cpp" "carousel_launcher.cpp" "${BOARD_DIR}/board.cpp"
    INCLUDE_DIRS "." "${BOARD_DIR}" "${CMAKE_CURRENT_LIST_DIR}/../../../assets"
    REQUIRES fake08 z8lua LovyanGFX esp_driver_uart esp_driver_gpio esp_driver_ledc esp_driver_i2c esp_timer sdcard_spi fatfs sdmmc esp_driver_sdmmc input esp_lcd esp_mm esp_app_format esp_driver_usb_serial_jtag
)
# esp_lcd + esp_mm are for the ESP32-P4 board.cpp (MIPI-DSI panel: esp_lcd DSI bus/DBI/DPI + esp_cache_msync
# for the PSRAM framebuffer). The S3 board.cpp doesn't use them (LovyanGFX i80), but linking them there is
# harmless — the REQUIRES is the superset covering whichever board.cpp is compiled in.
# SD-over-SPI is now the board-agnostic components/sdcard_spi (fatfs/sdmmc/sdspi are its private
# concern); esp_driver_spi reaches board.cpp transitively via sdcard_spi's public REQUIRES.
# The main component compiles main.cpp (driver/uart.h) + board.cpp (driver/ledc.h, driver/i2c_master.h,
# driver/gpio.h). ESP-IDF v6 split the monolithic `driver` component, so each header's specific esp_driver_*
# is required directly (all exist on v5.3+ too, so this stays valid on v5.4.2).

# Opt-in Celeste build: `make build APP=pico-e32-fake08 ... DEFS='-D CELESTE=1'` embeds the real
# Celeste .p8 (the gitignored assets/celeste_p8.h) instead of the test cart. Off by default.
if(DEFINED CELESTE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CELESTE=${CELESTE})
endif()

# Opt-in dev/HITL autostart (`... DEFS='-D CELESTE=1 -D CELESTE_START=1'`): begin_game() + load_room(0,0) at
# boot so the play-test spawns straight into room (0,0), skipping the title (the fc-scheduled input backend
# takes only fc-tagged commands, not the raw title-start key). See main.cpp. Off by default.
if(DEFINED CELESTE_START)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CELESTE_START=${CELESTE_START})
endif()

# Opt-in fps measurement: `... DEFS='-D CELESTE=1 -D MEASURE_FPS=1'` runs an instrumented loop that
# times Step() vs drawFrame() over UART instead of fake-08's GameLoop. Off by default.
if(DEFINED MEASURE_FPS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MEASURE_FPS=${MEASURE_FPS})
endif()

# Opt-in dev "screenshot over serial" (`... DEFS='-D FB_DUMP=1'`, P4 only): streams the live DPI
# framebuffer once, framed, for a camera-free host PNG via tools/fb_screenshot.py. See main.cpp.
if(DEFINED FB_DUMP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FB_DUMP=${FB_DUMP})
endif()

# Opt-in on-screen FPS HUD (`... DEFS='-D SHOW_FPS=1'`). The fake08 component already forwards this to its
# ESP32Host generic (render-loop-rate) meter; main.cpp ALSO uses it: the TELEMETRY play-test loop takes over the
# HUD to draw the cart's true achieved game-frame fps instead, so main needs the define too. Off by default.
if(DEFINED SHOW_FPS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SHOW_FPS=${SHOW_FPS})
endif()

# Opt-in dev diagnostic (`... DEFS='-D SPIKE_HEAP_LOG=1'`, needs TELEMETRY): on a slow Step (>20 ms) print the
# heap free before->after that Step, so a GC pause (heap jumps UP) is distinguishable from pure cart compute.
if(DEFINED SPIKE_HEAP_LOG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SPIKE_HEAP_LOG=${SPIKE_HEAP_LOG})
endif()

# Opt-in bench utility (`... DEFS='-D LCD_FILL=1'` for white, or `-D LCD_FILL=0xRRGGBB`): flood the panel one
# solid colour then idle forever, turning an idle board into a fill light for the bench camera. See main.cpp.
if(DEFINED LCD_FILL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LCD_FILL=${LCD_FILL})
endif()

# Opt-in perf experiment (`... DEFS='-D GC_MANUAL=1'`, needs TELEMETRY; add `-D GC_CORE1=1` to offload to core 1):
# disable z8lua's auto collector and drive it in a bounded time slice each frame, so a big collection spreads
# (and, with GC_CORE1, hides behind the Lua-free blit). Tunables: GC_BUDGET_US, GC_STEP_KB, GC_GUARD_KB.
foreach(gcflag GC_MANUAL GC_CORE1 GC_BUDGET_US GC_STEP_KB GC_GUARD_KB GC_GEN GC_MAJORINC LUABENCH)
    if(DEFINED ${gcflag})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${gcflag}=${${gcflag}})
    endif()
endforeach()

# Opt-in dev/demo flag: `... DEFS='-D FORCE_FLASH_CART=1'` ignores any SD cart and runs the flash cart
# (e.g. the input-reactive test cart) even when the microSD holds a .p8. Off by default.
if(DEFINED FORCE_FLASH_CART)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE FORCE_FLASH_CART=${FORCE_FLASH_CART})
endif()

# Opt-in launcher/menu: `... DEFS='-D LAUNCHER=1'` boots fake-08's built-in cart browser (the DefaultCart)
# when an SD card is mounted, so the user navigates the card's carts/folders and picks one, instead of
# auto-loading. Pairs with INPUT_BACKEND=touch (the deck drives the browser). Off by default.
if(DEFINED LAUNCHER)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LAUNCHER=${LAUNCHER})
endif()

# Opt-in serial position telemetry: `... DEFS='-D CELESTE=1 -D TELEMETRY=1'` runs a GameLoop variant that
# prints the Celeste player position + current room each frame over UART (via ExecuteLua/printh) for
# hardware-in-the-loop input solving/verification. Pairs with INPUT_BACKEND=serial. Off by default.
if(DEFINED TELEMETRY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TELEMETRY=${TELEMETRY})
endif()

# Opt-in racer telemetry tail: `... DEFS='-D TELEMETRY=1 -D RACER=1'` streams Pico Racer's state
# (tpos/px/speed/clock/starttimer) instead of Celeste's player/room. Off by default. See main.cpp.
if(DEFINED RACER)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE RACER=${RACER})
endif()

# Opt-in PRNG pin: `... DEFS='-D RND_SEED=39'` seeds PICO-8's rnd() deterministically after boot, so a cart
# with rnd()-driven state (Pico Racer's traffic) replays exactly against a host trace built at the same seed.
if(DEFINED RND_SEED)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE RND_SEED=${RND_SEED})
endif()

# Opt-in host-set telemetry tail (DEV/HITL ONLY — compiled out of production): `-D TELEMETRY_HOST_CFG=1` makes
# the TELEMETRY build read the cart-state expression from the host over serial at startup (a "CFG?" prompt +
# a `TT<expr>` reply), so the firmware carries no per-cart telemetry. Requires TELEMETRY. See main.cpp.
if(DEFINED TELEMETRY_HOST_CFG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TELEMETRY_HOST_CFG=${TELEMETRY_HOST_CFG})
endif()

# Opt-in higher telemetry baud (DEV/HITL): `-D TELEMETRY_BAUD=921600` bumps the console UART to this rate for
# the run (the CFG handshake stays at the boot default), cutting per-frame serialization time + jitter. The
# host switches to match after it reads the board's "BAUD <n>" announce. Requires TELEMETRY_HOST_CFG.
if(DEFINED TELEMETRY_BAUD)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TELEMETRY_BAUD=${TELEMETRY_BAUD})
endif()

# Opt-in BINARY telemetry (DEV/HITL): `-D TELEMETRY_BINARY=1 -D TELEMETRY_BINARY_BYTES=40`. The host-set tail
# becomes a POKE statement writing int16 fields into scratch RAM 0x4300..; the firmware ships them raw + sync-
# framed instead of an ASCII line — fewer bytes, no fix32->string/GC. Requires TELEMETRY_HOST_CFG. See main.cpp.
if(DEFINED TELEMETRY_BINARY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TELEMETRY_BINARY=${TELEMETRY_BINARY})
endif()
if(DEFINED TELEMETRY_BINARY_BYTES)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE TELEMETRY_BINARY_BYTES=${TELEMETRY_BINARY_BYTES})
endif()
