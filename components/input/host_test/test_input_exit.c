/* Host test of the IN-6 exit-gesture timing (components/input/input_exit.c) against stub ESP-IDF headers.
 *
 * This logic has been wrong twice, in opposite directions, and neither bug was reachable from the bench: the
 * gesture cannot be exercised over serial (USB-Serial-JTAG batches input at ~1 Hz, so a held button is
 * unrepresentable), and the board it ships on is the one that cannot be driven that way. So the timing is
 * pinned here instead, with a fake clock. Cases 4 and 5 are the two historical bugs.
 */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

static int64_t g_now_us   = 0;
static int     g_restarts = 0;
int64_t esp_timer_get_time(void) { return g_now_us; }
void    esp_restart(void)        { g_restarts++; }

#include "input_exit.c"   /* found via -I .. (run.sh); pulls in the static state under test */

#define FRAME_US 16667                       /* one 60 Hz poll */
#define MENU     INPUT_PAUSE

/* Reset the helper's static run state between cases: an un-held poll past the gap clears it. */
static void reset_state(void) {
    input_exit_enable(true);
    g_now_us += (int64_t)INPUT_EXIT_MAX_GAP_MS * 1000 * 4;
    input_exit_check(0);
    g_restarts = 0;
}
static void poll_for(int64_t total_us, uint8_t held, int64_t step_us) {
    for (int64_t t = 0; t < total_us; t += step_us) { g_now_us += step_us; input_exit_check(held); }
}

int main(void) {
    /* 1. A short hold must not fire — it has to keep meaning PICO-8's pause. */
    reset_state();
    poll_for(800 * 1000, MENU, FRAME_US);
    assert(g_restarts == 0);

    /* 2. A full hold fires exactly once. */
    reset_state();
    poll_for((INPUT_EXIT_HOLD_MS + 200) * 1000, MENU, FRAME_US);
    assert(g_restarts == 1);

    /* 3. Disarmed (the launcher's own screens) never fires, however long it is held. */
    reset_state();
    input_exit_enable(false);
    poll_for(3000 * 1000, MENU, FRAME_US);
    assert(g_restarts == 0);
    input_exit_enable(true);

    /* 4. REGRESSION (first review round): a tap, then a long stall, then one more held poll. A serial byte
     *    holds MENU for several polls, and carousel_fb_dump()/GC/SD can block for seconds; comparing only two
     *    held polls turned that into a reboot from a tap. */
    reset_state();
    input_exit_check(MENU);                  /* the tap */
    g_now_us += 5000 * 1000;                 /* the stall — far past the 1.2 s threshold */
    input_exit_check(MENU);
    assert(g_restarts == 0);

    /* 5. REGRESSION (second review round): the P4's shipped touch backend returns 0 whenever the GT911 has no
     *    new frame, so a real hold arrives as held polls interleaved with empty ones. Resetting the run on any
     *    single empty poll made the gesture almost unfirable on the board it ships on. */
    reset_state();
    for (int64_t t = 0; t < (INPUT_EXIT_HOLD_MS + 200) * 1000; t += FRAME_US * 2) {
        g_now_us += FRAME_US; input_exit_check(MENU);   /* controller posted a frame */
        g_now_us += FRAME_US; input_exit_check(0);      /* buffer-not-ready: NOT a release */
    }
    assert(g_restarts == 1);

    /* 6. A genuine release does end the run: silence longer than the gap, then a fresh press, must restart the
     *    measurement rather than resume the old one. */
    reset_state();
    poll_for(1000 * 1000, MENU, FRAME_US);              /* nearly there... */
    poll_for((INPUT_EXIT_MAX_GAP_MS + 100) * 1000, 0, FRAME_US);   /* ...released */
    poll_for(800 * 1000, MENU, FRAME_US);               /* a new, still-too-short hold */
    assert(g_restarts == 0);

    printf("input_exit.c: compiles clean + all 6 timing assertions PASS "
           "(hold=%d ms, gap=%d ms)\n", INPUT_EXIT_HOLD_MS, INPUT_EXIT_MAX_GAP_MS);
    return 0;
}
