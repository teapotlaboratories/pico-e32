/* Headless Celeste benchmark: runs PicoPico's celeste.p8 game logic on z8lua with
 * the PICO-8 draw/input/audio API stubbed (no-op), map+sprite-flags loaded so
 * objects spawn and collision runs. Measures ms/frame of _update+_draw (pure Lua). */
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "celeste_cart.h"
#include <cstdio>
#include <cstdint>
#include <cstring>

/* ---- data-backed PICO-8 API (C) ---- */
static int toint(lua_State *L, int i) { return (int)(int32_t)luaL_checknumber(L, i); }

static int l_mget(lua_State *L) {
    int x = toint(L,1), y = toint(L,2);
    uint8_t t = (x>=0&&x<128&&y>=0&&y<32) ? CELESTE_MAP[y*128+x] : 0;
    lua_pushnumber(L, z8::fix32((int)t)); return 1;
}
static int l_mset(lua_State *L) {
    int x=toint(L,1), y=toint(L,2), v=toint(L,3);
    if (x>=0&&x<128&&y>=0&&y<32) CELESTE_MAP[y*128+x]=(uint8_t)v;
    return 0;
}
static int l_fget(lua_State *L) {
    int n=toint(L,1);
    uint8_t fl=(n>=0&&n<256)?CELESTE_GFF[n]:0;
    if (lua_gettop(L)>=2) { int f=toint(L,2); lua_pushboolean(L,(fl>>f)&1); }
    else lua_pushnumber(L, z8::fix32((int)fl));
    return 1;
}
/* scripted active input so the player runs/jumps/dashes (representative object churn:
 * dashes spawn particles, deaths/respawns reload the room). btn indices: 0L 1R 2U 3D 4jump 5dash. */
static int g_frame = 0, g_jump_period = 20, g_dash_period = 28;
static int l_btn(lua_State *L) {
    int i = toint(L,1), f = g_frame; bool p=false;
    switch (i) {
        case 1: p = (f%64) < 40; break;              /* right ~60% */
        case 0: p = (f%64) >= 40 && (f%64) < 50; break; /* left occasionally */
        case 2: p = (f%40) < 12; break;              /* up (climb) */
        case 4: p = (f%g_jump_period) < 4; break;    /* jump */
        case 5: p = (f%g_dash_period) < 2; break;    /* dash (spawns particles — the expensive action) */
        default: p = false;
    }
    lua_pushboolean(L, p); return 1;
}
static uint32_t s_rng = 0x2a13c4f7u;
static int l_rnd(lua_State *L) {
    s_rng = s_rng*1103515245u + 12345u;
    double r = (double)(s_rng>>8) / (double)(1u<<24);   /* [0,1) */
    z8::fix32 x = (lua_gettop(L)>=1 && !lua_isnil(L,1)) ? luaL_checknumber(L,1) : z8::fix32(1);
    lua_pushnumber(L, x * z8::fix32(r)); return 1;
}

/* ---- Lua preamble: table helpers + no-op stubs ---- */
static const char *PREAMBLE = R"lua(
local function nop() end
spr=nop sspr=nop rectfill=nop rect=nop circfill=nop circ=nop line=nop pset=nop
print=nop cls=nop camera=nop pal=nop palt=nop clip=nop color=nop fillp=nop map=nop
sfx=nop music=nop pget=function() return 0 end
function btnp(i) return false end   -- btn() is a C function (scripted input)
function add(t,v) t[#t+1]=v return v end
function del(t,v) for i=1,#t do if t[i]==v then table.remove(t,i) return end end end
function foreach(t,f) for i=1,#t do local v=t[i] if v~=nil then f(v) end end end
function count(t) return #t end
function all(t) local i=0 return function() i=i+1 return t[i] end end
sub=string.sub
max=max min=min  -- from lpico8lib
)lua";

static void reg(lua_State *L, const char *name, lua_CFunction f) {
    lua_pushcfunction(L, f); lua_setglobal(L, name);
}
static bool run(lua_State *L, const char *chunk, const char *what) {
    if (luaL_loadstring(L, chunk) || lua_pcall(L, 0, 0, 0)) {
        printf("  ERROR in %s: %s\n", what, lua_tostring(L, -1)); lua_pop(L,1); return false;
    }
    return true;
}
static bool call(lua_State *L, const char *fn) {
    lua_getglobal(L, fn);
    if (!lua_isfunction(L, -1)) { lua_pop(L,1); return true; } /* cart may lack _draw etc. */
    if (lua_pcall(L, 0, 0, 0)) { printf("  ERROR in %s(): %s\n", fn, lua_tostring(L,-1)); lua_pop(L,1); return false; }
    return true;
}

/* Average active-input _update+_draw ms/frame over gameplay levels 3..15 (each reloaded,
 * warmed, then timed). Uses the current g_jump_period / g_dash_period input profile. */
static double bench_pass(lua_State *L, uint64_t (*now_us)(void), double *plo, double *phi) {
    const int WARM=10, PER=40;
    uint64_t total=0; int totframes=0; double lo=1e18, hi=0;
    for (int lv=3; lv<=15; lv++) {
        lua_getglobal(L,"load_room");
        lua_pushnumber(L, z8::fix32(lv%8)); lua_pushnumber(L, z8::fix32(lv/8));
        if (lua_pcall(L,2,0,0)) { printf("  load L%d err: %s\n", lv, lua_tostring(L,-1)); lua_pop(L,1); continue; }
        for (int w=0;w<WARM;w++){ g_frame=w; call(L,"_update"); call(L,"_draw"); }
        uint64_t t0=now_us();
        for (int i=0;i<PER;i++){ g_frame=WARM+i; call(L,"_update"); call(L,"_draw"); }
        uint64_t dt=now_us()-t0;
        double lvms=(double)dt/1000.0/PER; if(lvms<lo)lo=lvms; if(lvms>hi)hi=lvms;
        total+=dt; totframes+=PER;
    }
    *plo=lo; *phi=hi;
    return (double)total/1000.0/totframes;
}

/* run_celeste(now_us): frames of _update+_draw, prints ms/frame. Returns 0 ok. */
extern "C" int run_celeste(uint64_t (*now_us)(void)) {
    lua_State *L = luaL_newstate();
    if (!L) { printf("newstate failed\n"); return 1; }
    luaL_openlibs(L);
    reg(L,"mget",l_mget); reg(L,"mset",l_mset); reg(L,"fget",l_fget); reg(L,"rnd",l_rnd);
    reg(L,"btn",l_btn);
    if (!run(L, PREAMBLE, "preamble")) { lua_close(L); return 1; }
    if (!run(L, CELESTE_LUA, "celeste cart")) { lua_close(L); return 1; }
    if (!call(L, "_init")) { lua_close(L); return 1; }
    if (!call(L, "begin_game")) { lua_close(L); return 1; }   /* skip title -> load room 0, spawn player */

    /* survey object density per level (all levels 0-15 are in __map__) */
    printf("celeste: level object counts: ");
    for (int lv=0; lv<16; lv++) {
        lua_getglobal(L,"load_room");
        lua_pushnumber(L, z8::fix32(lv%8)); lua_pushnumber(L, z8::fix32(lv/8));
        if (lua_pcall(L,2,0,0)) { printf("[L%d err:%s] ", lv, lua_tostring(L,-1)); lua_pop(L,1); continue; }
        lua_getglobal(L,"objects"); printf("L%d=%d ", lv, lua_istable(L,-1)?(int)lua_rawlen(L,-1):-1); lua_pop(L,1);
    }
    printf("\n");

    /* Average active-input _update+_draw over gameplay levels 3-15. (Input intensity and
     * GC pause were both measured to have ~no effect; see worklog 2026-07-14 gate2.) */
    double lo, hi;
    g_jump_period=45; g_dash_period=110;   /* typical input */
    double ms = bench_pass(L, now_us, &lo, &hi);
    printf("celeste: %.3f ms/frame avg  (levels 3-15, per-level %.2f-%.2f ms, active input, graphics stubbed)\n",
           ms, lo, hi);
    printf("  budgets: 30fps<=33.3ms, 60fps<=16.6ms\n");
    lua_close(L);
    return 0;
}

#ifdef CELESTE_HOST_MAIN
#include <chrono>
static uint64_t host_now(){ using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count(); }
int main(){ return run_celeste(host_now); }
#endif
