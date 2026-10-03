/* Samsung S3C2400 manual ch.10: TCMPB controls PWM duty, not the
 * countdown/reload/IRQ period. Timer 4 has no compare register. */
#include "s3c2400.h"
#include <stdio.h>

static int check_clock_phase(s3c2400_t *s, uint32_t control, uint32_t irq_mask) {
    s3c2400_reset(s);
    s3c2400_write32(s, 0x14800004u, 0x3000u); /* FCLK 66 MHz. */
    s3c2400_write32(s, 0x14800014u, 0u);
    for (unsigned t = 0; t < 5u; ++t) s3c2400_write32(s, 0x1510000cu + t * 12u, 160u);
    s3c2400_write32(s, 0x15100008u, control);
    s3c2400_tick(s, 161u); /* Half of the 322-cycle period. */
    for (unsigned step = 0; step < 2u; ++step) {
        s3c2400_write32(s, 0x14800014u, step ? 0u : 3u);
        /* Divider 3 doubles CPU cycles per PCLK timer tick. The counter
         * must stay half-way through, in both directions of the switch. */
        for (unsigned t = 0; t < 5u; ++t) {
            uint32_t reg = t == 4u ? 0x15100040u : 0x15100014u + t * 12u;
            if (s3c2400_read32(s, reg) != 80u) goto fail;
        }
        uint32_t remaining = step ? 161u : 322u;
        s3c2400_tick(s, remaining - 1u);
        if (s3c2400_read32(s, 0x14400000u) & irq_mask) goto fail;
        s3c2400_tick(s, 1u);
        if ((s3c2400_read32(s, 0x14400000u) & irq_mask) != irq_mask) goto fail;
        s3c2400_write32(s, 0x14400000u, irq_mask);
        if (!step) s3c2400_tick(s, 322u);
    }
    return 1;
fail:
    fputs("FAIL: PWM clock change rewinds counter or shifts IRQ boundary\n", stderr);
    return 0;
}

int main(void) {
    static const uint32_t start[] = {1u, 0x100u, 0x1000u, 0x10000u, 0x100000u};
    static const uint32_t reload[] = {8u, 0x800u, 0x8000u, 0x80000u, 0x400000u};
    static const uint32_t compare[] = {0u, 80u, 110u, 200u};
    const uint32_t irq_mask = 0x1fu << 10;
    s3c2400_t *s = s3c2400_create(8u * 1024u * 1024u);
    if (!s) return 2;
    uint32_t control = 0;
    for (unsigned t = 0; t < 5; ++t) {
        s3c2400_write32(s, 0x1510000cu + t * 12u, 160u);
        if (t < 4) s3c2400_write32(s, 0x15100010u + t * 12u, compare[t]);
        control |= start[t] | reload[t];
    }
    s3c2400_write32(s, 0x15100008u, control);
    unsigned events = 0;
    for (unsigned cycle = 0; cycle < 1200; ++cycle) {
        /* Changing only the duty buffer during a period must not rephase it. */
        if (cycle == 100) s3c2400_write32(s, 0x15100010u, 155u);
        s3c2400_tick(s, 1);
        uint32_t pending = s3c2400_read32(s, 0x14400000u) & irq_mask;
        if (pending && pending != irq_mask) {
            fprintf(stderr, "FAIL: compare-dependent IRQ cycle=%u pending=%08x\n", cycle + 1u, pending);
            s3c2400_destroy(s); return 1;
        }
        uint32_t count = s3c2400_read32(s, 0x15100040u);
        for (unsigned t = 0; t < 4; ++t)
            if (s3c2400_read32(s, 0x15100014u + t * 12u) != count) {
                fprintf(stderr, "FAIL: compare-dependent countdown timer=%u cycle=%u\n", t, cycle + 1u);
                s3c2400_destroy(s); return 1;
            }
        if (pending) {
            ++events;
            s3c2400_write32(s, 0x14400000u, irq_mask);
        }
    }
    int phase_ok = check_clock_phase(s, control, irq_mask);
    s3c2400_destroy(s);
    if (!phase_ok) return 1;
    if (events < 2u) { fputs("FAIL: timer reload was not exercised\n", stderr); return 1; }
    puts("PASS: PWM duty changes preserve countdown and repeated IRQ timing across all timers");
    return 0;
}
