# Native build of the z8lua Gate #2 benchmark (verification aid; runs on the dev host).
# z8lua ships as upstream .c, but LUA_NUMBER is z8::fix32 (a C++ type), so the core is
# compiled AS C++ (-x c++) with setjmp/longjmp error handling -- no files are renamed.
CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -DLUA_USE_LONGJMP -w
Z8       := ../../../components/z8lua
INC      := -I$(Z8) -I../bench

# Compile the Lua core, skipping the units linit never opens (they pull host OS / dlopen
# deps) -- mirrors the exclude list in the component's CMakeLists.txt.
Z8_SRC   := $(filter-out %/liolib.c %/loslib.c %/loadlib.c %/ltests.c,$(wildcard $(Z8)/*.c))
SRC      := ../bench/bench.cpp host_main.cpp

bench: $(Z8_SRC) $(SRC)
	$(CXX) $(CXXFLAGS) $(INC) -x c++ $(Z8_SRC) $(SRC) -o bench -lm

run: bench
	./bench

clean:
	rm -f bench

.PHONY: run clean
