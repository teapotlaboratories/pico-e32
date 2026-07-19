// Headless desktop Host for fake-08 — the platform-specific half (the shared half is the vendored
// source/hostCommonFunctions.cpp). Input is scripted (not a keyboard), drawFrame captures the 128x128
// framebuffer, and timing/audio are no-ops so the VM can be single-stepped. Mirrors the structure of the
// vendored test/stubhost.cpp; nothing in the vendored tree is modified. Also stubs the logger (the real
// logger.cpp is excluded from this build, like the fake-08 test build).
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
using namespace std;

#include "host.h"
#include "hostVmShared.h"
#include "logger.h"

static uint8_t  g_kdown = 0, g_kheld = 0;
static uint8_t  g_fb[128 * 64];          // 128x128, nibble-packed (2 px/byte)
static uint8_t  g_pal[64];               // screen palette map snapshot
static bool     g_haveframe = false;

extern "C" void simhost_set_input(uint8_t kdown, uint8_t kheld) { g_kdown = kdown; g_kheld = kheld; }
extern "C" const uint8_t* simhost_fb()  { return g_fb; }
extern "C" const uint8_t* simhost_pal() { return g_pal; }
extern "C" int            simhost_haveframe() { return g_haveframe ? 1 : 0; }

// --- logger stubs (real logger.cpp excluded from the build) ---
void Logger_Initialize(const char*) {}
void Logger_Write(const char*, ...) {}
void Logger_WriteUnformatted(const char*) {}

// --- platform Host methods (the shared ones live in hostCommonFunctions.cpp) ---
Host::Host(int, int) {}
void Host::oneTimeSetup(Audio*) {}
void Host::oneTimeCleanup() {}
void Host::setTargetFps(int) {}
void Host::changeStretch() {}
void Host::forceStretch(StretchOption) {}

InputState_t Host::scanInput() {
    return InputState_t{ g_kdown, g_kheld, 0, 0, 0, false, "" };
}
bool Host::shouldQuit() { return false; }
void Host::waitForTargetFps() {}

void Host::drawFrame(uint8_t* picoFb, uint8_t* screenPaletteMap, uint8_t) {
    memcpy(g_fb, picoFb, sizeof(g_fb));
    if (screenPaletteMap) memcpy(g_pal, screenPaletteMap, sizeof(g_pal));
    g_haveframe = true;
}

bool   Host::shouldFillAudioBuff()   { return false; }
void*  Host::getAudioBufferPointer() { return nullptr; }
size_t Host::getAudioBufferSize()    { return 0; }
void   Host::playFilledAudioBuffer() {}
bool   Host::shouldRunMainLoop()     { return true; }

vector<string> Host::listcarts()          { return {}; }
vector<string> Host::listdirs()           { return {}; }
string Host::customBiosLua()              { return ""; }
string Host::getCartDirectory()           { return "carts"; }

double Host::deltaTMs()                    { return 33.0; }
void   Host::overrideLogFilePrefix(const char* p) { _logFilePrefix = p ? p : ""; }
const char* Host::logFilePrefix()          { return _logFilePrefix.c_str(); }
