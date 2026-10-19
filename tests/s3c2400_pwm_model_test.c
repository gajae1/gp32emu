/* Independent timer-edge oracle for Samsung S3C2400 ch.10, especially
 * pp.10-3--10-6 and 10-10. The oracle walks individual clock edges; the
 * implementation is free to skip whole intervals. No private SoC state or
 * production timing helper is used to compute an expected value.
 *
 * The documented sequence is positive count -> zero/IRQ + capture TCNTB ->
 * next timer edge/reload. The exact asynchronous start/divider phase is not
 * specified: these tests use the emulator policy of a full first tick and a
 * retained fraction across manual update and stop. A manually loaded zero
 * stays zero until another manual load; it is not an undocumented 65536.
 */
#include "s3c2400.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define PWM 0x15100000u
#define RAM 0x0c000000u
#define IRQ 0x14400000u
#define TIMER_IRQS (0x1fu << 10)

static const uint32_t start[5] = {1u, 0x100u, 0x1000u, 0x10000u, 0x100000u};
static const uint32_t reload[5] = {8u, 0x800u, 0x8000u, 0x80000u, 0x400000u};
static unsigned cases, checks, failures;
static const char *scenario;

static void expect(uint32_t actual, uint32_t wanted, unsigned timer, const char *what) {
    ++checks;
    if (actual == wanted) return;
    if (failures < 48u)
        fprintf(stderr, "FAIL: %s timer=%u %s actual=%u expected=%u\n",
                scenario, timer, what, actual, wanted);
    ++failures;
}

static uint32_t count_address(unsigned timer) {
    return PWM + (timer == 4u ? 0x40u : 0x14u + timer * 12u);
}

static uint32_t current(s3c2400_t *s, unsigned timer) {
    return s3c2400_read32(s, count_address(timer));
}

static void clock_48(s3c2400_t *s) {
    s3c2400_reset(s);
    s3c2400_write32(s, 0x14800004u, 0u);
    s3c2400_write32(s, 0x14800014u, 0u);
}

static void buffer_write(s3c2400_t *s, unsigned timer, uint32_t value, unsigned width) {
    uint32_t addr = PWM + 0xcu + timer * 12u;
    if (width == 1u) s3c2400_write8(s, addr, (uint8_t)value);
    else if (width == 2u) s3c2400_write16(s, addr, (uint16_t)value);
    else s3c2400_write32(s, addr, value);
}

static void manual_load(s3c2400_t *s, unsigned timer, uint32_t control) {
    s3c2400_write32(s, PWM + 8u, control | (start[timer] << 1));
    s3c2400_write32(s, PWM + 8u, control);
}

static void control_lane_write(s3c2400_t *s, unsigned timer, uint32_t value, unsigned width) {
    unsigned byte = timer == 0u ? 0u : timer < 3u ? 1u : 2u;
    unsigned lane = width == 4u ? 0u : width == 2u ? byte & ~1u : byte;
    if (width == 1u) s3c2400_write8(s, PWM + 8u + lane, (uint8_t)(value >> (lane * 8u)));
    else if (width == 2u) s3c2400_write16(s, PWM + 8u + lane, (uint16_t)(value >> (lane * 8u)));
    else s3c2400_write32(s, PWM + 8u, value);
}

static void check_manual_lanes(s3c2400_t *s) {
    scenario = "manual update access lanes";
    for (unsigned timer = 0; timer < 5u; ++timer)
        for (unsigned width = 1u; width <= 4u; width *= 2u) {
            ++cases;
            clock_48(s);
            buffer_write(s, timer, UINT32_MAX, 4u);
            control_lane_write(s, timer, reload[timer] | (start[timer] << 1), width);
            expect(current(s, timer), 65535u, timer, "manual load masks buffer to sixteen bits");
            control_lane_write(s, timer, start[timer] | reload[timer], width);
            s3c2400_tick(s, 1u);
            buffer_write(s, timer, 13u, 4u);
            control_lane_write(s, timer, reload[timer] | start[timer] * 3u, width);
            expect(current(s, timer), 13u, timer, "selected TCON lane performs manual load");
            control_lane_write(s, timer, start[timer] | reload[timer], width);
            s3c2400_tick(s, 1u);
            expect(current(s, timer), 12u, timer, "lane manual load retains half tick");
            control_lane_write(s, timer, reload[timer], width);
            s3c2400_tick(s, 100u);
            expect(current(s, timer), 12u, timer, "lane stop retains count");
            control_lane_write(s, timer, start[timer] | reload[timer], width);
            s3c2400_tick(s, 2u);
            expect(current(s, timer), 11u, timer, "lane restart resumes count");
        }
}

static void check_buffer_stop_manual(s3c2400_t *s) {
    scenario = "buffer/stop/manual";
    for (unsigned timer = 0; timer < 5u; ++timer)
        for (unsigned width = 1u; width <= 4u; width *= 2u) {
            ++cases;
            clock_48(s);
            buffer_write(s, timer, 100u, width);
            expect(current(s, timer), 0u, timer, "buffer alone does not load counter");
            manual_load(s, timer, reload[timer]);
            expect(current(s, timer), 100u, timer, "manual load while stopped");
            s3c2400_write32(s, PWM + 8u, start[timer] | reload[timer]);
            s3c2400_tick(s, 21u); /* Ten decrements and half a timer tick. */
            expect(current(s, timer), 90u, timer, "count after ten clocks");
            buffer_write(s, timer, 200u, width);
            expect(current(s, timer), 90u, timer, "buffer write retains live count");
            s3c2400_write32(s, PWM + 8u, reload[timer]);
            expect(current(s, timer), 90u, timer, "stop retains live count");
            s3c2400_tick(s, 137u);
            expect(current(s, timer), 90u, timer, "stopped clocks retain count");
            s3c2400_write32(s, PWM + 8u, start[timer] | reload[timer]);
            s3c2400_tick(s, 1u);
            expect(current(s, timer), 89u, timer, "restart retains fractional tick");
            buffer_write(s, timer, 50u, width);
            manual_load(s, timer, start[timer] | reload[timer]);
            expect(current(s, timer), 50u, timer, "running manual load");
            s3c2400_tick(s, 3u);
            expect(current(s, timer), 49u, timer, "running loaded counter advances");
            buffer_write(s, timer, 17u, width);
            manual_load(s, timer, start[timer] | reload[timer]);
            s3c2400_tick(s, 1u);
            expect(current(s, timer), 16u, timer, "manual update retains sub-tick policy");
        }
}

static void check_terminal_latch(s3c2400_t *s) {
    scenario = "terminal/latch";
    for (unsigned timer = 0; timer < 5u; ++timer) {
        ++cases;
        clock_48(s);
        uint32_t irq = 1u << (10u + timer), control = start[timer] | reload[timer];
        buffer_write(s, timer, 3u, 4u);
        manual_load(s, timer, control);
        buffer_write(s, timer, 7u, 4u);
        s3c2400_tick(s, 5u);
        expect(current(s, timer), 1u, timer, "one before terminal");
        expect(s3c2400_read32(s, IRQ) & irq, 0u, timer, "no early request");
        s3c2400_tick(s, 1u);
        expect(current(s, timer), 0u, timer, "zero is observable");
        expect(s3c2400_read32(s, IRQ) & irq, irq, timer, "request when counter reaches zero");
        s3c2400_write32(s, IRQ, irq);
        buffer_write(s, timer, 11u, 4u); /* An ISR's write after the latch. */
        s3c2400_tick(s, 1u);
        expect(current(s, timer), 0u, timer, "zero holds until next timer edge");
        s3c2400_tick(s, 1u);
        expect(current(s, timer), 7u, timer, "reload uses value latched at zero");
        expect(s3c2400_read32(s, IRQ) & irq, 0u, timer, "reload itself is not another request");
        s3c2400_tick(s, 14u);
        expect(current(s, timer), 0u, timer, "new counter reaches zero");
        expect(s3c2400_read32(s, IRQ) & irq, irq, timer, "next interval request");
        s3c2400_tick(s, 2u);
        expect(current(s, timer), 11u, timer, "following interval uses ISR buffer");

        /* A forced stop in the observable zero interval retains its captured
         * reload. A new manual update explicitly replaces that pending load. */
        s3c2400_tick(s, 22u);
        s3c2400_write32(s, PWM + 8u, reload[timer]);
        buffer_write(s, timer, 19u, 4u);
        s3c2400_tick(s, 20u);
        expect(current(s, timer), 0u, timer, "stopped zero phase holds");
        s3c2400_write32(s, PWM + 8u, control);
        s3c2400_tick(s, 2u);
        expect(current(s, timer), 11u, timer, "stopped zero phase resumes captured reload");
        s3c2400_tick(s, 22u);
        buffer_write(s, timer, 23u, 4u);
        manual_load(s, timer, control);
        s3c2400_tick(s, 2u);
        expect(current(s, timer), 22u, timer, "manual load cancels pending reload");

        /* One-shot completion stops at zero and cannot acquire a new buffer
         * from a bare start. Zero initialization is a documented model policy. */
        buffer_write(s, timer, 2u, 4u);
        manual_load(s, timer, start[timer]);
        s3c2400_write32(s, IRQ, irq);
        s3c2400_tick(s, 4u);
        expect(current(s, timer), 0u, timer, "one-shot stops at zero");
        expect(s3c2400_read32(s, IRQ) & irq, irq, timer, "one-shot terminal request");
        s3c2400_write32(s, IRQ, irq);
        s3c2400_write32(s, PWM + 8u, start[timer]);
        s3c2400_tick(s, 100u);
        expect(current(s, timer), 0u, timer, "bare restart cannot reload exhausted counter");
        expect(s3c2400_read32(s, IRQ) & irq, 0u, timer, "exhausted counter does not invent requests");
        buffer_write(s, timer, 0u, 4u);
        manual_load(s, timer, control);
        s3c2400_tick(s, 2u);
        expect(current(s, timer), 0u, timer, "literal zero initialization");
    }
}

typedef struct oracle {
    uint32_t buffer[5], count[5], latched[5], phase[5], period[5];
    unsigned running[5], repeat[5], pending[5];
    uint32_t irqs, control, dma_values[32], dma_used;
    unsigned dma_timer, dma_kind;
} oracle_t;

static void oracle_control(oracle_t *o, uint32_t control) {
    o->control = control;
    for (unsigned t = 0; t < 5u; ++t) {
        o->running[t] = !!(control & start[t]);
        o->repeat[t] = !!(control & reload[t]);
        if (control & (start[t] << 1)) {
            o->count[t] = o->buffer[t];
            o->latched[t] = o->pending[t] = 0u;
        }
    }
}

/* One indivisible reference clock quantum, chosen as a common divisor of
 * RUN and PCLK. All counters change before the same-instant request bus runs.
 * There is intentionally no modulo shortcut for multiple timer intervals. */
static void oracle_clock(oracle_t *o) {
    unsigned due = 0u;
    for (unsigned t = 0; t < 5u; ++t) {
        if (!o->running[t] || (!o->count[t] && !o->pending[t])) continue;
        if (++o->phase[t] != o->period[t]) continue;
        o->phase[t] = 0u;
        if (o->pending[t]) {
            o->count[t] = o->latched[t];
            o->pending[t] = o->latched[t] = 0u;
        } else if (--o->count[t] == 0u) {
            due |= 1u << t;
            if (o->repeat[t]) {
                o->latched[t] = o->buffer[t];
                o->pending[t] = 1u;
            } else {
                o->running[t] = 0u;
                o->control &= ~start[t];
            }
        }
    }
    for (unsigned t = 0; t < 5u; ++t) {
        if (!(due & (1u << t))) continue;
        if (o->dma_timer == t + 1u) {
            if (o->dma_used < 12u) {
                if (o->dma_kind == 1u) o->dma_values[o->dma_used] = o->count[1];
                else if (o->dma_kind == 2u) o->buffer[0] = 5u + o->dma_used * 2u;
                ++o->dma_used;
            }
        } else o->irqs |= 1u << (10u + t);
    }
}

static void oracle_cycles(oracle_t *o, unsigned cycles, unsigned clock_units) {
    for (unsigned c = 0; c < cycles; ++c)
        for (unsigned edge = 0; edge < clock_units; ++edge) oracle_clock(o);
}

static void compare_oracle(s3c2400_t *s, const oracle_t *o) {
    for (unsigned t = 0; t < 5u; ++t) expect(current(s, t), o->count[t], t, "edge oracle count");
    expect(s3c2400_read32(s, IRQ) & TIMER_IRQS, o->irqs, 5u, "edge oracle pending requests");
    expect(s3c2400_read32(s, PWM + 8u), o->control, 5u, "edge oracle control");
}

static void check_edge_oracle(s3c2400_t *s) {
    for (unsigned fractional = 0; fractional < 2u; ++fractional) {
        scenario = fractional ? "fractional edge oracle" : "integer edge oracle";
        ++cases;
        oracle_t o = {0};
        clock_48(s);
        if (fractional) {
            s3c2400_write32(s, 0x14800004u, 0xe000u); /* RUN 48 MHz, PCLK 66 MHz. */
            s3c2400_write32(s, 0x14800014u, 2u);
        }
        const unsigned clock_units = fractional ? 11u : 1u;
        uint32_t control = 0u;
        for (unsigned t = 0; t < 5u; ++t) {
            o.period[t] = fractional ? 16u : 2u;
            o.buffer[t] = 31u + t * 7u;
            buffer_write(s, t, o.buffer[t], 4u);
            control |= start[t] | reload[t] | (start[t] << 1);
        }
        oracle_control(&o, control);
        s3c2400_write32(s, PWM + 8u, control);
        control &= ~0x222202u;
        oracle_control(&o, control);
        s3c2400_write32(s, PWM + 8u, control);
        uint32_t random = 0x125738abu;
        for (unsigned action = 0; action < 240u; ++action) {
            random = random * 1664525u + 1013904223u;
            unsigned t = random % 5u, kind = (random >> 8) % 4u;
            control = o.control;
            if (kind == 0u || kind == 3u) {
                o.buffer[t] = 1u + (random >> 16) % 97u;
                buffer_write(s, t, o.buffer[t], 2u);
                if (kind == 3u) control |= start[t] << 1;
            } else if (kind == 1u) control ^= start[t];
            else control ^= reload[t];
            oracle_control(&o, control);
            s3c2400_write32(s, PWM + 8u, control);
            control &= ~0x222202u;
            oracle_control(&o, control);
            s3c2400_write32(s, PWM + 8u, control);
            unsigned cycles = 1u + (random >> 20) % 47u;
            oracle_cycles(&o, cycles, clock_units);
            s3c2400_tick(s, cycles);
            compare_oracle(s, &o);
            o.irqs = 0u;
            s3c2400_write32(s, IRQ, TIMER_IRQS);
        }
    }
}

static void check_dma_order(s3c2400_t *s) {
    for (unsigned kind = 1u; kind <= 2u; ++kind)
        for (unsigned partition = 0u; partition < 3u; ++partition) {
            scenario = kind == 1u ? "DMA samples another live timer" : "DMA buffer after terminal latch";
            ++cases;
            clock_48(s);
            oracle_t o = {0};
            o.dma_kind = kind;
            o.dma_timer = 1u;
            uint32_t control = 0u;
            for (unsigned t = 0; t < 5u; ++t) {
                o.period[t] = 2u;
                o.buffer[t] = 3u + t * 2u;
                buffer_write(s, t, o.buffer[t], 4u);
                control |= start[t] | reload[t] | (start[t] << 1);
            }
            oracle_control(&o, control);
            s3c2400_write32(s, PWM + 8u, control);
            control &= ~0x222202u;
            oracle_control(&o, control);
            s3c2400_write32(s, PWM + 8u, control);
            s3c2400_write32(s, PWM + 4u, 1u << 20); /* Timer0 DMA requests. */
            for (unsigned i = 0; i < 12u; ++i) s3c2400_write32(s, RAM + 0x100u + 4u*i, 5u + 2u*i);
            s3c2400_write32(s, 0x14600000u, kind == 1u ? 0x20000000u | count_address(1u) : RAM + 0x100u);
            s3c2400_write32(s, 0x14600004u, kind == 1u ? RAM + 0x200u : 0x20000000u | (PWM + 0xcu));
            s3c2400_write32(s, 0x14600008u, (3u << 24) | (1u << 23) | (1u << 22) | (2u << 20) | 12u);
            s3c2400_write32(s, 0x14600018u, 2u);
            for (unsigned elapsed = 0u; elapsed < 701u;) {
                unsigned n = partition == 0u ? 701u : partition == 1u ? 1u : 37u;
                if (n > 701u - elapsed) n = 701u - elapsed;
                oracle_cycles(&o, n, 1u);
                s3c2400_tick(s, n);
                elapsed += n;
            }
            compare_oracle(s, &o);
            expect(s3c2400_read32(s, 0x1460000cu), 0u, 0u, "all twelve DMA requests serviced");
            if (kind == 1u)
                for (unsigned i = 0; i < 12u; ++i)
                    expect(s3c2400_read32(s, RAM + 0x200u + i*4u), o.dma_values[i], 0u, "chronological DMA sample");
        }
}

static void check_dma_start(s3c2400_t *s) {
    scenario = "DMA starts stopped timer within tick";
    for (unsigned partition = 0u; partition < 3u; ++partition) {
        ++cases;
        clock_48(s);
        buffer_write(s, 0u, 3u, 4u);
        manual_load(s, 0u, 9u);
        buffer_write(s, 1u, 20u, 4u);
        manual_load(s, 1u, 9u); /* Loaded but stopped. */
        buffer_write(s, 0u, 7u, 4u);
        s3c2400_write32(s, PWM + 4u, 1u << 20);
        s3c2400_write32(s, RAM, start[1]);
        s3c2400_write32(s, 0x14600000u, RAM);
        s3c2400_write32(s, 0x14600004u, 0x20000000u | (PWM + 8u));
        s3c2400_write32(s, 0x14600008u, (3u << 24) | (1u << 23) | (1u << 22) | (2u << 20) | 1u);
        s3c2400_write32(s, 0x14600018u, 2u);
        for (unsigned elapsed = 0u; elapsed < 25u;) {
            unsigned n = partition == 0u ? 25u : partition == 1u ? 1u : 7u;
            if (n > 25u - elapsed) n = 25u - elapsed;
            s3c2400_tick(s, n);
            elapsed += n;
        }
        /* The request is at CPU cycle 6. Only the remaining nineteen cycles
         * may advance timer 1: nine decrements and one retained half tick. */
        expect(current(s, 0u), 0u, 0u, "DMA stops terminal timer at zero");
        expect(current(s, 1u), 11u, 1u, "DMA-started timer sees only subsequent clocks");
        expect(s3c2400_read32(s, PWM + 8u), start[1], 5u, "DMA changed start controls");
        expect(s3c2400_read32(s, IRQ) & TIMER_IRQS, 0u, 5u, "selected DMA replaces timer IRQ");
        s3c2400_tick(s, 1u);
        expect(current(s, 1u), 10u, 1u, "DMA-started timer retains fraction");
        buffer_write(s, 0u, 11u, 4u);
        s3c2400_write32(s, PWM + 8u, start[0]);
        s3c2400_tick(s, 2u);
        expect(current(s, 0u), 7u, 0u, "DMA stop retained already captured reload");
    }
}

static void check_dma_ack_order(s3c2400_t *s) {
    scenario = "DMA acknowledges previously pending timer IRQ";
    for (unsigned partition = 0u; partition < 3u; ++partition) {
        ++cases;
        clock_48(s);
        buffer_write(s, 0u, 10u, 4u);
        manual_load(s, 0u, start[0]);
        buffer_write(s, 1u, 2u, 4u);
        manual_load(s, 1u, start[0] | start[1] | reload[1]);
        s3c2400_write32(s, PWM + 4u, 1u << 20);
        s3c2400_write32(s, RAM, TIMER_IRQS);
        s3c2400_write32(s, 0x14600000u, RAM);
        s3c2400_write32(s, 0x14600004u, 0x20000000u | IRQ);
        s3c2400_write32(s, 0x14600008u, (3u << 24) | (1u << 23) | (1u << 22) | (2u << 20) | 1u);
        s3c2400_write32(s, 0x14600018u, 2u);
        for (unsigned elapsed = 0u; elapsed < 20u;) {
            unsigned n = partition == 0u ? 20u : partition == 1u ? 1u : 7u;
            if (n > 20u - elapsed) n = 20u - elapsed;
            s3c2400_tick(s, n);
            elapsed += n;
        }
        /* Timer 1 requested at cycles 4, 10 and 16. Timer 0's DMA at cycle
         * 20 acknowledges those old requests; timer 1 is currently at one.
         * Coalescing already pending requests must not defer them past ACK. */
        expect(current(s, 1u), 1u, 1u, "acknowledged timer has not reached next zero");
        expect(s3c2400_read32(s, IRQ) & TIMER_IRQS, 0u, 5u, "DMA clears all earlier requests");
        s3c2400_tick(s, 2u);
        expect(s3c2400_read32(s, IRQ) & TIMER_IRQS, 1u << 11, 5u, "next terminal requests again");
    }
}

static void check_large_tick(s3c2400_t *s) {
    scenario = "maximum tick with pending IRQ";
    ++cases;
    clock_48(s);
    buffer_write(s, 0u, 1u, 4u);
    manual_load(s, 0u, 9u);
    s3c2400_tick(s, UINT32_MAX);
    /* First zero is cycle 2, then 6, 10, ...; last full edge is odd. */
    expect(current(s, 0u), 0u, 0u, "large tick ends in zero interval");
    expect(s3c2400_read32(s, IRQ) & (1u << 10), 1u << 10, 0u, "large tick retains pending IRQ");
    s3c2400_tick(s, 1u);
    expect(current(s, 0u), 1u, 0u, "large tick retains fractional edge");
}

int main(int argc, char **argv) {
    s3c2400_t *s = s3c2400_create(4096u);
    if (!s) return 2;
    check_buffer_stop_manual(s);
    check_manual_lanes(s);
    check_terminal_latch(s);
    check_edge_oracle(s);
    check_dma_order(s);
    check_dma_start(s);
    check_dma_ack_order(s);
    /* The pre-change model loops over every IRQ period. The separately
     * selected stress case is first run once the bounded scheduler exists. */
    if (argc > 1 && !strcmp(argv[1], "large")) check_large_tick(s);
    s3c2400_destroy(s);
    printf("PWM model scenarios=%u checks=%u assertion_failures=%u\n", cases, checks, failures);
    return failures ? 1 : 0;
}
