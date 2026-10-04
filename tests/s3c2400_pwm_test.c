/* Samsung S3C2400 manual ch.10: TCMPB controls PWM duty, not the
 * countdown/reload/IRQ period. Timer 4 has no compare register. */
#include "s3c2400.h"
#include <stdio.h>
#include <string.h>

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


/* A real ARM handler must observe the same timer expirations regardless of
 * the host CPU slice. Delayed guest enable also checks pre-write time. */
typedef struct timer_result {
    uint32_t regs[16], cpsr, count, pending, control;
} timer_result_t;

static int run_timer_cpu(unsigned quantum, int jit, unsigned timer,
                         int reload, int delayed, timer_result_t *out) {
    s3c2400_t *s = s3c2400_create(8u * 1024u * 1024u);
    if (!s) return 0;
    arm_bus_t bus = s3c2400_get_bus(s);
    arm920t_t *c = arm920t_create(&bus);
    if (!c) { s3c2400_destroy(s); return 0; }
    s3c2400_set_irq_sink(s, c);
    arm920t_set_jit(c, jit);
    uint8_t bios[32] = {0};
    gp32_st32le(bios + 0x18u, 0xe51ff004u);
    gp32_st32le(bios + 0x1cu, 0x0c001000u);
    char error[128];
    int ok = s3c2400_load_bios_buffer(s, bios, sizeof(bios), error, sizeof(error));
    if (!ok) goto end;
    const uint32_t handler[] = {
        0xe5823000u, /* str r3,[r2]: acknowledge SRCPND */
        0xe5823010u, /* str r3,[r2,#16]: acknowledge INTPND */
        0xe2855001u, /* add r5,r5,#1 */
        0xe25ef004u, /* subs pc,lr,#4 */
    };
    const uint32_t enable[] = {
        0xe3a06064u, /* mov r6,#100 */
        0xe2566001u, /* subs r6,r6,#1 */
        0x1afffffdu, /* bne to subs */
        0xe5801000u, /* str r1,[r0]: enable timer after elapsed CPU work */
        0xeafffffeu,
    };
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(handler); ++i)
        s3c2400_write32(s, 0x0c001000u + i*4u, handler[i]);
    for (unsigned i = 0; i < GP32_ARRAY_COUNT(enable); ++i)
        s3c2400_write32(s, 0x0c000000u + i*4u, enable[i]);
    if (!delayed) s3c2400_write32(s, 0x0c000000u, 0xeafffffeu);
    uint32_t control = timer ? 0x100000u : 1u;
    if (reload) control |= timer ? 0x400000u : 8u;
    s3c2400_write32(s, 0x14800004u, 0u);
    s3c2400_write32(s, 0x14800014u, 0u);
    s3c2400_write32(s, 0x1510000cu + timer*12u, 499u);
    if (!delayed) s3c2400_write32(s, 0x15100008u, control);
    s3c2400_write32(s, 0x14400008u, ~(1u << (10u + timer)));
    arm920t_set_cpsr(c, 0x53u);
    arm920t_set_reg(c, 0u, 0x15100008u);
    arm920t_set_reg(c, 1u, control);
    arm920t_set_reg(c, 2u, 0x14400000u);
    arm920t_set_reg(c, 3u, 1u << (10u + timer));
    arm920t_set_reg(c, 15u, 0x0c000000u);
    for (unsigned remaining = 5000u; remaining;) {
        unsigned step = remaining < quantum ? remaining : quantum;
        uint32_t done = s3c2400_run_cpu(s, step);
        if (!done || done > step) { ok = 0; goto end; }
        remaining -= done;
    }
    for (unsigned i = 0; i < 16; ++i) out->regs[i] = arm920t_get_reg(c, i);
    out->cpsr = arm920t_get_cpsr(c);
    out->count = s3c2400_read32(s, timer ? 0x15100040u : 0x15100014u);
    out->pending = s3c2400_read32(s, 0x14400000u);
    out->control = s3c2400_read32(s, 0x15100008u);
end:
    arm920t_destroy(c); s3c2400_destroy(s); return ok;
}

static int check_cpu_slices(void) {
    for (int jit = 0; jit <= 1; ++jit)
        for (unsigned timer = 0; timer <= 4; timer += 4)
            for (int reload = 0; reload <= 1; ++reload)
                for (int delayed = 0; delayed <= 1; ++delayed) {
                    timer_result_t fine = {0}, coarse = {0};
                    if (!run_timer_cpu(1, jit, timer, reload, delayed, &fine) ||
                        !run_timer_cpu(32768, jit, timer, reload, delayed, &coarse)) return 0;
                    if (fine.regs[5] != (reload ? 4u : 1u) ||
                        memcmp(&fine, &coarse, sizeof(fine))) {
                        fprintf(stderr, "FAIL: timer=%u jit=%d reload=%d delayed=%d IRQ=%u/%u count=%u/%u\n",
                                timer, jit, reload, delayed, fine.regs[5], coarse.regs[5], fine.count, coarse.count);
                        return 0;
                    }
                }
    return 1;
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
    if (!phase_ok || !check_cpu_slices()) return 1;
    if (events < 2u) { fputs("FAIL: timer reload was not exercised\n", stderr); return 1; }
    puts("PASS: PWM duty changes preserve countdown and repeated IRQ timing across all timers");
    return 0;
}
