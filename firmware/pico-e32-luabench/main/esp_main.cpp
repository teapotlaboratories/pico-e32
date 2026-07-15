/* Phase-0 Gate #2 on the ESP32-S3: z8lua interpreter throughput.
 *
 * Runs the shared benchmark twice — Lua heap in internal SRAM vs in PSRAM — so
 * you can read off the placement penalty (the plan keeps the hot Lua state in
 * SRAM for exactly this reason). Prints ms + M iter/s per case over UART.
 *
 * Gate #2 pass: a representative frame's Lua work fits in <= 33 ms (30 fps floor),
 * ideally <= 16.6 ms. Compare the SRAM column to that budget. */

#include "bench.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static uint64_t now_us(void) { return (uint64_t)esp_timer_get_time(); }

/* Custom Lua allocator that forces a fixed heap-caps class (SRAM or PSRAM). */
static void *lua_alloc_caps(void *ud, void *ptr, size_t osize, size_t nsize)
{
  (void)osize;
  uint32_t caps = (uint32_t)(uintptr_t)ud;
  if (nsize == 0) { if (ptr) heap_caps_free(ptr); return NULL; }
  return heap_caps_realloc(ptr, nsize, caps);
}

static void run_with_caps(uint32_t caps, const char *label)
{
  lua_State *L = lua_newstate(lua_alloc_caps, (void *)(uintptr_t)caps);
  if (!L) { printf("  lua_newstate(%s) failed — not enough memory of that class\n", label); return; }
  luaL_openlibs(L);
  run_cases(L, now_us, label);
  lua_close(L);
}

extern "C" void app_main(void)
{
  printf("\n=== pico-e32 Phase-0 Gate #2 — z8lua throughput ===\n");
  esp_chip_info_t ci; esp_chip_info(&ci);
  printf("chip: ESP32-S3 rev%d, %d core(s)\n", ci.revision, ci.cores);
  printf("heap: internal free %u B, PSRAM free %u B\n",
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  run_with_caps(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, "heap = internal SRAM");
  run_with_caps(MALLOC_CAP_SPIRAM   | MALLOC_CAP_8BIT, "heap = PSRAM (quad on the ILI9488 board)");

  printf("\n=== Celeste (real cart: celeste.p8 game logic, graphics/audio/input stubbed) ===\n");
  run_celeste(now_us);

  printf("\nnote: numbers are 16.16 fixed point; loop bounds must be < 32767.\n");
  printf("Gate #2: SRAM total <= 33 ms/frame-of-work => pass at 30 fps.\n");
  printf("=== done ===\n");
  while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}
