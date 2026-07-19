# Native (desktop) build of the fake-08 VM into libfake08sim.so, for solving/verifying Celeste plans on
# the exact same VM the device runs. Links the vendored fake-08 source + z8lua without modifying them
# (z8lua .c compile as C++, per the component; logger.cpp + main.cpp are replaced by our stubs, like the
# fake-08 test build). See README.md.
CXX ?= g++
F08  := ../../../components/fake08/fake08
SRC  := $(F08)/source
Z8   := ../../../components/z8lua
LODE := $(F08)/libs/lodepng
UTF8 := $(F08)/libs/utf8-util
MINIZ:= $(F08)/libs/miniz
SINI := $(F08)/libs/simpleini

INC  := -I$(SRC) -I$(F08)/include -I$(Z8) -I$(LODE) -I$(UTF8) -I$(MINIZ) -I$(SINI)
# EXTRA_CXXFLAGS / EXTRA_LDFLAGS: debug hook, e.g. a sanitizer build:
#   make clean && make EXTRA_CXXFLAGS='-fsanitize=address -g -O0' EXTRA_LDFLAGS='-fsanitize=address'
CXXFLAGS := -O2 -fPIC -std=c++17 -w -DLUA_USE_LONGJMP $(INC) $(EXTRA_CXXFLAGS)

# fake-08 core (minus logger.cpp + main.cpp — we stub those, as the vendored test build does)
CORE_CPP := $(filter-out $(SRC)/logger.cpp $(SRC)/main.cpp,$(wildcard $(SRC)/*.cpp))
LIB_CPP  := $(wildcard $(LODE)/*.cpp) $(wildcard $(UTF8)/*.cpp) $(wildcard $(MINIZ)/*.cpp)
# z8lua Lua core (minus host-OS / test-only units) — compiled as C++ (LUA_NUMBER = z8::fix32)
Z8_C     := $(filter-out $(Z8)/liolib.c $(Z8)/loslib.c $(Z8)/loadlib.c $(Z8)/ltests.c,$(wildcard $(Z8)/*.c))
MINIZ_C  := $(wildcard $(MINIZ)/*.c)
MINE     := host_sim.cpp sim.cpp

BUILD := build
OBJ := $(patsubst %,$(BUILD)/%.o,$(notdir $(CORE_CPP)) $(notdir $(LIB_CPP)) $(notdir $(MINE))) \
       $(patsubst %,$(BUILD)/%.o,$(notdir $(Z8_C)) $(notdir $(MINIZ_C)))
VPATH := $(SRC):$(LODE):$(UTF8):$(MINIZ):$(Z8):.

libfake08sim.so: $(OBJ)
	$(CXX) -shared -o $@ $(OBJ) $(EXTRA_LDFLAGS)

$(BUILD)/%.cpp.o: %.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(BUILD)/%.c.o: %.c | $(BUILD)
	$(CXX) $(CXXFLAGS) -x c++ -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD) libfake08sim.so
.PHONY: clean
