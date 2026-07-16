# Native build of the ESP32Host cart runner (verification aid; runs on the dev host).
#
# host_main.cpp compiles for two targets from one source: the ESP32 (via idf.py, drawing to
# the ILI9488) and here (-DHOST_MAIN, no display). Same cart, same draw API, same
# framebuffer -- so the per-frame fb_hash values must match the device's, which is how
# rendering is verified WITHOUT the bench camera. See docs/worklog/2026-07-14-phase0-gate3-host.md.
#
#   make run                 # run 10 frames, print fb_hash per frame
#   make dump                # also write frames to frames/ and render one to PNG
#   make asan                # same, with ASan/UBSan (caught a Lua-stack bug pre-flash)
#
# Mirrors firmware/pico-e32-luabench/host/Makefile: z8lua ships as upstream .c but
# LUA_NUMBER is z8::fix32 (a C++ type), so the core is compiled AS C++ (-x c++) -- no files
# are renamed (.ai/AGENTS.md -> least-destructive vendor edits).
CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -DLUA_USE_LONGJMP -DHOST_MAIN -w
Z8       := ../../../components/z8lua
INC      := -I$(Z8)

# Skip the units linit never opens (they pull host OS / dlopen deps) -- mirrors the
# exclude list in the component's CMakeLists.txt.
Z8_SRC   := $(filter-out %/liolib.c %/loslib.c %/loadlib.c %/ltests.c,$(wildcard $(Z8)/*.c))
SRC      := ../main/host_main.cpp

host: $(Z8_SRC) $(SRC)
	$(CXX) $(CXXFLAGS) $(INC) -x c++ $(Z8_SRC) $(SRC) -o host -lm

run: host
	./host

# Dump raw frames and render the last one, so the framebuffer can be *looked at*.
# The hash proves host and device agree; only the image proves either is right.
dump: host
	@mkdir -p frames
	P8_DUMP=frames ./host
	../../../tools/p8_png.py frames/frame_009.raw frames/frame_009.png

asan: CXXFLAGS += -fsanitize=address,undefined -g -O1
asan: clean host
	./host

clean:
	rm -f host
	rm -rf frames

.PHONY: run dump asan clean
