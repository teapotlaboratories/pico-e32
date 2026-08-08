/* Host-test stub: the real esp_restart() never returns. The test defines it to count invocations instead,
 * which is exactly the observable the exit-gesture logic is judged on. */
#pragma once
void esp_restart(void);
