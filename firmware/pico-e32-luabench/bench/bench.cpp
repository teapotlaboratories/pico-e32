#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "bench.h"
#include <stdio.h>

/* z8lua's LUA_NUMBER is z8::fix32 (16.16 fixed point, range ~+-32768). Loop
 * bounds must stay < 32767, so each chunk keeps a small inner loop and we drive
 * the repetition count from C. The chunk is compiled once (luaL_loadstring) and
 * executed `reps` times, so we time pure interpreter execution, not parsing. */

struct Case { const char *name; const char *code; long inner; long reps; };

static const Case CASES[] = {
  { "empty loop (FORLOOP only)",
    "local s=0 for i=1,10000 do end return s", 10000, 200 },                  /* pure loop dispatch overhead */
  { "integer add only",
    "local s=0 for i=1,10000 do s=s+i end return s", 10000, 200 },            /* loop + 1 fix32 add, no mul/div */
  { "fixed-point arithmetic",
    "local s=0 for i=1,10000 do s=s+i*0.5-i/3 end return s", 10000, 200 },   /* 2.0M ops  */
  { "arith, no divide",
    "local s=0 for i=1,10000 do s=s+i*0.5-i*0.33 end return s", 10000, 200 }, /* same shape, i*0.33 not i/3: isolates fix32 divide cost */
  { "pico-8 sin/cos sweep",
    "local s=0 for i=1,10000 do s=s+sin(i*0.0001)-cos(i*0.0002) end return s", 10000, 20 }, /* 200k */
  { "table churn (alloc+read)",
    "local t={} for i=1,10000 do t[i]={x=i,y=-i} end "
    "local s=0 for i=1,10000 do s=s+t[i].x-t[i].y end return s", 10000, 10 }, /* 100k */

  /* --- profile: isolate the speedup levers (docs/reference/z8lua-speedup-research.md) --- */
  { "sin/cos, LOCALIZED",   /* vs "pico-8 sin/cos sweep" (globals) => globals->locals delta on Xtensa */
    "local sin,cos=sin,cos local s=0 for i=1,10000 do s=s+sin(i*0.0001)-cos(i*0.0002) end return s", 10000, 20 },
  { "table field r/w",      /* GETFIELD/SETFIELD pointer-chasing cost vs registers */
    "local t={x=0,y=0} local s=0 for i=1,10000 do t.x=t.x+i t.y=t.y-i s=s+t.x-t.y end return s", 10000, 50 },
  { "representative frame",  /* 64 objects: table index + field r/w + add + branch, light on divide */
    "local e={} for i=1,64 do e[i]={x=i,y=i,dx=1,dy=-1} end "
    "local s=0 for i=1,10000 do local o=e[i%64+1] "
    "o.x=o.x+o.dx if o.x>120 then o.dx=-o.dx end "
    "o.y=o.y+o.dy if o.y>120 then o.dy=-o.dy end s=s+o.x-o.y end return s", 10000, 20 },
};

extern "C" void run_cases(lua_State *L, uint64_t (*now_us)(void), const char *label)
{
  printf("\n[%s]\n", label);
  printf("%-26s %11s %13s\n", "case", "ms", "M iter/s");
  printf("-------------------------------------------------\n");
  double total_ms = 0;
  for (const auto &c : CASES) {
    if (luaL_loadstring(L, c.code)) {                       /* compile once */
      printf("  COMPILE ERROR %-11s %s\n", c.name, lua_tostring(L, -1)); lua_pop(L, 1); continue;
    }
    lua_pushvalue(L, -1);                                   /* warm-up */
    if (lua_pcall(L, 0, 0, 0)) { printf("  RUN ERROR %-15s %s\n", c.name, lua_tostring(L, -1)); lua_pop(L, 2); continue; }
    uint64_t t0 = now_us();
    for (long r = 0; r < c.reps; ++r) { lua_pushvalue(L, -1); lua_pcall(L, 0, 0, 0); }
    uint64_t t1 = now_us();
    lua_pop(L, 1);                                          /* pop compiled chunk */
    double us = (double)(t1 - t0);
    total_ms += us / 1000.0;
    printf("%-26s %11.2f %13.3f\n", c.name, us / 1000.0,
           ((double)c.inner * c.reps / (us / 1e6)) / 1e6);
  }
  printf("-------------------------------------------------\n");
  printf("total %.2f ms   (frame budget: 16.6 ms @60fps / 33.3 ms @30fps)\n", total_ms);
}

extern "C" void run_bench(uint64_t (*now_us)(void))
{
  lua_State *L = luaL_newstate();
  if (!L) { printf("luaL_newstate failed\n"); return; }
  luaL_openlibs(L);
  printf("z8lua Gate #2 bench  (LUA_NUMBER=z8::fix32, Lua 5.2)\n");
  run_cases(L, now_us, "default heap");
  printf("note: numbers are 16.16 fixed point; loop bounds must be < 32767.\n");
  lua_close(L);
}
