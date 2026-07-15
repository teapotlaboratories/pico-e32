#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Phase-0 Gate #2 microbenchmark for z8lua (PICO-8's fixed-point Lua dialect).
 * The goal is to measure raw interpreter throughput on the ESP32-S3 (LX7) and
 * decide whether a faithful PICO-8 runtime can hit the 30 fps floor.
 *
 * now_us: a monotonic microsecond clock (esp_timer_get_time on device,
 *         std::chrono on host). */

struct lua_State;

/* Run the benchmark cases on an already-opened Lua state, tagged with `label`
 * (e.g. "heap=internal SRAM" vs "heap=PSRAM" to expose the placement delta). */
void run_cases(struct lua_State *L, uint64_t (*now_us)(void), const char *label);

/* Convenience for the host build: create a default state, open libs, run, close. */
void run_bench(uint64_t (*now_us)(void));

/* Real-cart benchmark: runs PicoPico's celeste.p8 game logic (graphics/audio/input
 * stubbed, map+flags loaded) and prints ms/frame of _update+_draw averaged over
 * gameplay levels. The representative "does a real cart fit the frame budget" number. */
int run_celeste(uint64_t (*now_us)(void));

#ifdef __cplusplus
}
#endif
