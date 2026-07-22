/* Host compile + logic test of the firmware backend components/input/input_scheduled.c, against stub ESP-IDF
 * headers. Validates the C compiles cleanly AND that its ring/parser/apply-by-fc logic matches the Python twin
 * (fc_sched.py / test_fc_sched.py) on the same vectors. fc_rx_task is never run; we call the static feed_bytes
 * directly (the ring's producer), then input_set_frame + input_poll (the consumer), like the real flow. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "input_scheduled.c"   /* found via -I .. (run.sh) */     /* pulls in the static feed_bytes/ring + the seam */

#define L 1
#define R 2
#define U 4
#define D 8
#define O 16
#define X 32

static void enc(uint8_t *out, uint32_t fc, uint8_t mask, uint8_t hold) {
    out[0] = 0xA6;
    memcpy(out + 1, &fc, 4);
    out[5] = mask; out[6] = hold;
    uint8_t c = 0;
    for (int i = 0; i < 7; i++) c ^= out[i];
    out[7] = c;
}

int main(void) {
    input_init();                                   /* sets s_ok, "creates" the (stubbed) task */
    uint8_t pkt[8], two[16];

    /* apply-at-fc + expire */
    enc(pkt, 100, R, 1); feed_bytes(pkt, 8);
    input_set_frame(96);  assert(input_poll() == 0);        /* inserted (100>96); not active at 96 */
    input_set_frame(100); assert(input_poll() == R);        /* applies at the target fc */
    input_set_frame(102); assert(input_poll() == 0);        /* expired after 2*hold */

    /* deadline miss: arrives after its target frame */
    enc(pkt, 200, X, 2); feed_bytes(pkt, 8);
    input_set_frame(204); assert(input_poll() == 0);
    assert(s_miss == 1);
    input_set_frame(200); assert(input_poll() == 0);        /* a missed command never applies */

    /* multi-frame hold window [300, 300+2*3=306) */
    enc(pkt, 300, R, 3); feed_bytes(pkt, 8);
    input_set_frame(298); assert(input_poll() == 0);
    input_set_frame(300); assert(input_poll() == R);
    input_set_frame(302); assert(input_poll() == R);
    input_set_frame(304); assert(input_poll() == R);
    input_set_frame(306); assert(input_poll() == 0);

    /* a command drained exactly on its target frame is ON TIME (applies, not a miss) */
    enc(pkt, 600, R, 1); feed_bytes(pkt, 8);
    uint32_t miss0 = s_miss;
    input_set_frame(600); assert(input_poll() == R);
    assert(s_miss == miss0);

    /* OR of two concurrent commands, staggered expiry */
    enc(two, 400, R, 2); enc(two + 8, 400, U, 1);
    feed_bytes(two, 16);
    input_set_frame(399); assert(input_poll() == 0);        /* both future (399<400) */
    input_set_frame(400); assert(input_poll() == (R | U));  /* both active */
    input_set_frame(402); assert(input_poll() == R);        /* U expired, R held */

    /* resync past junk + a bad frame */
    uint8_t stream[64]; int n = 0;
    const char *junk = "log\n"; memcpy(stream + n, junk, 4); n += 4;
    enc(stream + n, 500, X, 1); n += 8;
    stream[n++] = 0xA6; stream[n++] = 0x00; stream[n++] = 'b'; stream[n++] = 'd';   /* a bad partial frame */
    enc(stream + n, 502, O, 1); n += 8;
    feed_bytes(stream, n);
    input_set_frame(499); assert(input_poll() == 0);
    input_set_frame(500); assert(input_poll() == X);        /* first valid recovered */
    input_set_frame(502); assert(input_poll() == O);        /* recovered after the bad frame */

    printf("input_scheduled.c: compiles clean + all logic assertions PASS "
           "(fed=%u miss=%u applied=%u)\n", s_fed, s_miss, s_applied);
    return 0;
}
