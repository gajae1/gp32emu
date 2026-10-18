/* Known model gaps, intentionally outside CTest. Samsung S3C2400 manual
 * pp.10-4--10-5 specifies reload buffering and counter retention at stop.
 * This probe avoids assuming the model's disputed terminal-count convention. */
#include "s3c2400.h"
#include <stdio.h>

static const uint32_t start[5] = {1u, 0x100u, 0x1000u, 0x10000u, 0x100000u};
static const uint32_t reload[5] = {8u, 0x800u, 0x8000u, 0x80000u, 0x400000u};
static unsigned failures;

static uint32_t current(s3c2400_t *s, unsigned timer) {
    return s3c2400_read32(s, timer == 4u ? 0x15100040u : 0x15100014u + timer * 12u);
}

static void start_timer(s3c2400_t *s, unsigned timer) {
    s3c2400_reset(s);
    s3c2400_write32(s, 0x14800004u, 0u); /* RUN=PCLK=48 MHz. */
    s3c2400_write32(s, 0x14800014u, 0u);
    s3c2400_write32(s, 0x1510000cu + timer * 12u, 100u);
    s3c2400_write32(s, 0x15100008u, start[timer] * 2u | reload[timer]);
    s3c2400_write32(s, 0x15100008u, start[timer] | reload[timer]);
    s3c2400_tick(s, 20u);
}

static void observation(const char *case_name, unsigned timer, uint32_t actual, uint32_t expected) {
    printf("timer=%u case=%s actual=%u expected=%u %s\n", timer, case_name,
           actual, expected, actual == expected ? "PASS" : "GAP");
    failures += actual != expected;
}

int main(void) {
    s3c2400_t *s = s3c2400_create(4096u);
    if (!s) return 2;
    for (unsigned timer = 0; timer < 5u; ++timer) {
        start_timer(s, timer);
        uint32_t before = current(s, timer);
        s3c2400_write32(s, 0x1510000cu + timer * 12u, 200u);
        observation("buffer-write-retains-count", timer, current(s, timer), before);

        start_timer(s, timer);
        before = current(s, timer);
        s3c2400_write32(s, 0x15100008u, reload[timer]);
        observation("stop-retains-count", timer, current(s, timer), before);
        s3c2400_tick(s, 200u);
        observation("stopped-count-stays-held", timer, current(s, timer), before);
        s3c2400_write32(s, 0x15100008u, start[timer] | reload[timer]);
        s3c2400_tick(s, 2u);
        observation("restart-resumes-count", timer, current(s, timer), before - 1u);

        start_timer(s, timer);
        s3c2400_write32(s, 0x1510000cu + timer * 12u, 50u);
        s3c2400_write32(s, 0x15100008u, start[timer] * 3u | reload[timer]);
        s3c2400_write32(s, 0x15100008u, start[timer] | reload[timer]);
        observation("manual-update-loads-new-count", timer, current(s, timer), 50u);
    }
    s3c2400_destroy(s);
    printf("PWM model-gap observations=25 mismatches=%u\n", failures);
    return failures ? 1 : 0;
}
