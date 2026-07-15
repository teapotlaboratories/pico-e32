/* Native (x86) build of the Gate #2 benchmark — a fast, verifiable sanity check
 * of z8lua + fix32 and the harness. The absolute numbers are NOT comparable to
 * the ESP32-S3; they only prove the runtime and methodology work. The real
 * Gate #2 number comes from `main/esp_main.cpp` on the device. */
#include "bench.h"
#include <chrono>

static uint64_t now_us(void)
{
  using namespace std::chrono;
  return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

int main(void)
{
  run_bench(now_us);
  return 0;
}
