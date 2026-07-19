// libfake08sim: drive the REAL fake-08 VM (same VM + cart + z8lua as the device) headlessly, for solving
// and verifying Celeste input plans on the desktop. C API (ctypes-friendly) — see fake08sim.py.
//
//   sim_init(cart_path)          boot + load the cart + run
//   sim_start_room(rx,ry)        begin_game() then load_room(rx,ry) -> spawn in that room
//   sim_step(held_mask)          advance ONE game-frame (2 coroutine resumes) with `held` buttons
//   sim_read(&found,&x,&y,&rx,&ry,&dj)   current player pos/room/djump (via ExecuteLua poke -> PicoRam)
//   sim_save() / sim_restore()   snapshot/restore the FULL VM state (Lua + PicoRam) — for the search
//   sim_frame_rgb(out)           128*128*3 RGB of the current frame (for the comparison video)
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "host.h"
#include "vm.h"
#include "PicoRam.h"
#include "Audio.h"
#include "hostVmShared.h"
#include "nibblehelpers.h"

extern "C" void simhost_set_input(uint8_t kdown, uint8_t kheld);
extern "C" const uint8_t* simhost_fb();
extern "C" const uint8_t* simhost_pal();

static Host*    host = nullptr;
static PicoRam* ram  = nullptr;
static Audio*   audio = nullptr;
static Vm*      vm   = nullptr;
static uint8_t  g_prev_held = 0;

// save slot (single is enough: snapshot the room spawn, restore per search candidate)
static char     g_lua[1 << 20];
static size_t   g_lua_len = 0;
static uint8_t  g_ram[0x10000];

// ExecuteLua snippet: stash the player + room into general-use RAM (0x4300-0x4305, which Celeste doesn't
// touch). y is offset by +64 so the exit (y<0) still fits a byte.
static const char* READ_LUA =
    "poke(0x4300,0) local p for o in all(objects) do if o.type==player then p=o end end "
    "if p then poke(0x4300,1) poke(0x4301,p.x) poke(0x4302,p.y+64) poke(0x4305,p.djump) "
    "poke(0x4307, p.spd.x>0 and 2 or (p.spd.x<0 and 0 or 1)) "
    "poke(0x4308, p.spd.y>0 and 2 or (p.spd.y<0 and 0 or 1)) end "
    "poke(0x4303,room.x) poke(0x4304,room.y) poke(0x4306,freeze or 0)";

extern "C" {

int sim_init(const char* cart_path) {
    FILE* f = fopen(cart_path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> buf(n);
    if (fread(buf.data(), 1, n, f) != (size_t)n) { fclose(f); return 0; }
    fclose(f);

    host = new Host(0, 0);
    ram = new PicoRam(); ram->Reset();
    audio = new Audio(ram);
    vm = new Vm(host, ram, nullptr, nullptr, audio);
    host->setUpPaletteColors();
    host->oneTimeSetup(audio);
    host->setTargetFps(60);
    if (!vm->LoadCart(buf.data(), buf.size(), false)) return 0;
    vm->vm_run();
    // Boot: run the cart's global scope + _init (sets up __cart_sandbox and the title) so ExecuteLua works.
    for (int i = 0; i < 60; i++) vm->Step();
    return 1;
}

// Put the player at room (rx,ry)'s spawn: begin_game() sets up game state (max_djump etc.), then
// load_room() teleports. Then run a couple of resumes so the player object exists.
void sim_start_room(int rx, int ry) {
    char snip[128];
    snprintf(snip, sizeof(snip), "begin_game() load_room(%d,%d)", rx, ry);
    vm->ExecuteLua(snip, "");
    vm->Step(); vm->Step();
    g_prev_held = 0;
}

void sim_exec(const char* lua) { vm->ExecuteLua(lua, ""); }
int  sim_peek(int addr)        { return ram->data[addr & 0xffff]; }

void sim_step(uint8_t held) {
    uint8_t kdown = held & ~g_prev_held;   // edge, like the device's scanInput (Celeste uses btn(), but
    simhost_set_input(kdown, held);         //   this keeps btnp() correct too)
    g_prev_held = held;
    vm->Step(); vm->Step();                 // 30 fps cart -> 2 resumes per game-frame
}

void sim_read(int* found, int* x, int* y, int* rx, int* ry, int* dj,
              int* fz, int* sx, int* sy) {
    vm->ExecuteLua(READ_LUA, "");
    const uint8_t* d = ram->data;
    *found = d[0x4300];
    *x  = d[0x4301];
    *y  = (int)d[0x4302] - 64;
    *rx = d[0x4303];
    *ry = d[0x4304];
    *dj = d[0x4305];
    *fz = d[0x4306];
    *sx = (int)d[0x4307] - 1;   // sign of spd.x
    *sy = (int)d[0x4308] - 1;   // sign of spd.y
}

// Per-node savestates for the search: the game state (player, objects, room) lives in the LUA state;
// PicoRam holds the constant map/sprites, so lua-only save/restore reproduces physics. Python holds one
// buffer per beam node.
int  sim_save_buf(char* out)            { return (int)vm->serializeLuaState(out); }
void sim_restore_buf(const char* in, int n) { vm->deserializeLuaState((char*)in, (size_t)n); g_prev_held = 0; }

// Step n game-frames with per-frame held masks, without reading — for a fast inner search loop.
void sim_steps(const unsigned char* held, int n) {
    for (int i = 0; i < n; i++) {
        uint8_t h = held[i];
        simhost_set_input(h & ~g_prev_held, h);
        g_prev_held = h;
        vm->Step(); vm->Step();
    }
}

void sim_save() {
    g_lua_len = vm->serializeLuaState(g_lua);
    memcpy(g_ram, ram->data, sizeof(g_ram));
}
void sim_restore() {
    vm->deserializeLuaState(g_lua, g_lua_len);
    memcpy(ram->data, g_ram, sizeof(g_ram));
    g_prev_held = 0;
}

// Render the current VM framebuffer through the Host (populates the capture buffer). GameLoop does this
// each frame; since we single-step, call it before sim_frame_rgb.
void sim_draw() { host->drawFrame(vm->GetPicoInteralFb(), vm->GetScreenPaletteMap(), 0); }

// 128x128x3 RGB of the last drawn frame (fb nibble -> screen palette -> RGB palette).
void sim_frame_rgb(uint8_t* out) {
    const uint8_t* fb = simhost_fb();
    const uint8_t* pal = simhost_pal();
    Color* colors = host->GetPaletteColors();
    for (int yy = 0; yy < 128; yy++) {
        for (int xx = 0; xx < 128; xx++) {
            uint8_t nib = getPixelNibble(xx, yy, fb);
            uint8_t pidx = pal[nib & 0x0f];
            Color c = colors[pidx];
            int o = (yy * 128 + xx) * 3;
            out[o] = c.Red; out[o + 1] = c.Green; out[o + 2] = c.Blue;
        }
    }
}

}  // extern "C"
