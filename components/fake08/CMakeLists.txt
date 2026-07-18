# IDF component wrapper for the vendored fake-08 fork.
#
# The fork (teapotlaboratories/fake-08 @ pico-e32) is a PURE vendored subtree at ./fake08 — it carries NO
# IDF build file. This parent-repo wrapper owns the whole ESP-IDF integration (which sources to compile,
# REQUIRES, compile flags), so the fork stays as close to upstream as possible and updating it is a clean
# submodule bump. The component directory is still "fake08", so `REQUIRES fake08` is unchanged.
#
# ./fake08/source is byte-identical to upstream fake-08 (the 1-to-1 rule); ./fake08/platform/esp32 is the
# additive ESP32 Host binding. The Lua VM is the repo's shared z8lua component (NOT fake-08's nested
# libs/z8lua, which is left un-inited). See docs/runtime/pico-e32-fake08-port.md.
set(F08 "${CMAKE_CURRENT_LIST_DIR}/fake08")

# fake-08's PICO-8 runtime (C++), minus the desktop entry point — the app provides app_main.
file(GLOB FAKE08_SRCS "${F08}/source/*.cpp")
list(FILTER FAKE08_SRCS EXCLUDE REGEX "/main\\.cpp$")

idf_component_register(
    SRCS ${FAKE08_SRCS}
         "${F08}/platform/esp32/ESP32Host.cpp"
         "${F08}/libs/lodepng/lodepng.cpp"
         "${F08}/libs/miniz/miniz.c"
         "${F08}/libs/simpleini/ConvertUTF.c"
    INCLUDE_DIRS "fake08/source" "fake08/platform/esp32"
                 "fake08/libs/lodepng" "fake08/libs/miniz" "fake08/libs/simpleini"
    REQUIRES z8lua esp_timer input
)

# Upstream third-party code: silence its warnings so a -Werror app build still passes.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)

# fake-08 is a C++17 codebase (its math helpers declare a global `lerp`, which collides with C++20's
# std::lerp under IDF's default newer standard). Compile it at gnu++17 — its intended standard — so
# ./fake08/source stays byte-identical. (Last -std wins.)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)

# lodepng disk I/O stays ENABLED: cart.cpp references lodepng's file overload unconditionally, so
# LODEPNG_NO_COMPILE_DISK would break the byte-identical source; that file path is simply never taken here.
# The board's board_lcd_* symbols (used by platform/esp32/ESP32Host.cpp via fake08_board.h) are defined in
# the app's board.cpp and resolved at the final app link.
