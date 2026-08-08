/* Host-test stub: the real esp_timer_get_time() is the monotonic microsecond clock. The test defines it so
 * input_exit.c's wall-clock logic can be driven against a controllable clock. */
#pragma once
#include <stdint.h>
int64_t esp_timer_get_time(void);
